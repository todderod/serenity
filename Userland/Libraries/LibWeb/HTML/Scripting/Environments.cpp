/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/PromiseRejectionEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/WindowEnvironmentSettingsObject.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/SecureContexts/AbstractOperations.h>

namespace Web::HTML {

EnvironmentSettingsObject::EnvironmentSettingsObject(NonnullOwnPtr<JS::ExecutionContext> realm_execution_context)
    : m_realm_execution_context(move(realm_execution_context))
{
    m_realm_execution_context->context_owner = this;

    // Register with the responsible event loop so we can perform step 4 of "perform a microtask checkpoint".
    responsible_event_loop().register_environment_settings_object({}, *this);
}

EnvironmentSettingsObject::~EnvironmentSettingsObject()
{
    responsible_event_loop().unregister_environment_settings_object({}, *this);
}

void EnvironmentSettingsObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    m_module_map = realm.heap().allocate_without_realm<ModuleMap>();
}

void EnvironmentSettingsObject::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(target_browsing_context);
    visitor.visit(m_module_map);
    visitor.ignore(m_outstanding_rejected_promises_weak_set);
    m_realm_execution_context->visit_edges(visitor);
}

JS::ExecutionContext& EnvironmentSettingsObject::realm_execution_context()
{
    // NOTE: All environment settings objects are created with a realm execution context, so it's stored and returned here in the base class.
    return *m_realm_execution_context;
}

ModuleMap& EnvironmentSettingsObject::module_map()
{
    return *m_module_map;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#environment-settings-object%27s-realm
JS::Realm& EnvironmentSettingsObject::realm()
{
    // An environment settings object's realm execution context's Realm component is the environment settings object's Realm.
    return *realm_execution_context().realm;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-global
JS::Object& EnvironmentSettingsObject::global_object()
{
    // An environment settings object's Realm then has a [[GlobalObject]] field, which contains the environment settings object's global object.
    return realm().global_object();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#responsible-event-loop
EventLoop& EnvironmentSettingsObject::responsible_event_loop()
{
    // An environment settings object's responsible event loop is its global object's relevant agent's event loop.
    // This is here in case the realm that is holding onto this ESO is destroyed before the ESO is. The responsible event loop pointer is needed in the ESO destructor to deregister from the event loop.
    // FIXME: Figure out why the realm can be destroyed before the ESO, as the realm is holding onto this with an OwnPtr, but the heap block deallocator calls the ESO destructor directly instead of through the realm destructor.
    if (m_responsible_event_loop)
        return *m_responsible_event_loop;

    auto& vm = global_object().vm();
    auto& event_loop = verify_cast<Bindings::WebEngineCustomData>(vm.custom_data())->event_loop;
    m_responsible_event_loop = &event_loop;
    return event_loop;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#check-if-we-can-run-script
RunScriptDecision EnvironmentSettingsObject::can_run_script()
{
    // 1. If the global object specified by settings is a Window object whose Document object is not fully active, then return "do not run".
    if (is<HTML::Window>(global_object()) && !verify_cast<HTML::Window>(global_object()).associated_document().is_fully_active())
        return RunScriptDecision::DoNotRun;

    // 2. If scripting is disabled for settings, then return "do not run".
    if (is_scripting_disabled())
        return RunScriptDecision::DoNotRun;

    // 3. Return "run".
    return RunScriptDecision::Run;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#prepare-to-run-script
void EnvironmentSettingsObject::prepare_to_run_script()
{
    // 1. Push settings's realm execution context onto the JavaScript execution context stack; it is now the running JavaScript execution context.
    global_object().vm().push_execution_context(realm_execution_context());

    // FIXME: 2. Add settings to the currently running task's script evaluation environment settings object set.
}

// https://html.spec.whatwg.org/multipage/webappapis.html#clean-up-after-running-script
void EnvironmentSettingsObject::clean_up_after_running_script()
{
    auto& vm = global_object().vm();

    // 1. Assert: settings's realm execution context is the running JavaScript execution context.
    VERIFY(&realm_execution_context() == &vm.running_execution_context());

    // 2. Remove settings's realm execution context from the JavaScript execution context stack.
    vm.pop_execution_context();

    // 3. If the JavaScript execution context stack is now empty, perform a microtask checkpoint. (If this runs scripts, these algorithms will be invoked reentrantly.)
    if (vm.execution_context_stack().is_empty())
        responsible_event_loop().perform_a_microtask_checkpoint();
}

static JS::ExecutionContext* top_most_script_having_execution_context(JS::VM& vm)
{
    // Here, the topmost script-having execution context is the topmost entry of the JavaScript execution context stack that has a non-null ScriptOrModule component,
    // or null if there is no such entry in the JavaScript execution context stack.
    auto execution_context = vm.execution_context_stack().last_matching([&](JS::ExecutionContext* context) {
        return !context->script_or_module.has<Empty>();
    });

    if (!execution_context.has_value())
        return nullptr;

    return execution_context.value();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#prepare-to-run-a-callback
void EnvironmentSettingsObject::prepare_to_run_callback()
{
    auto& vm = global_object().vm();

    // 1. Push settings onto the backup incumbent settings object stack.
    // NOTE: The spec doesn't say which event loop's stack to put this on. However, all the examples of the incumbent settings object use iframes and cross browsing context communication to demonstrate the concept.
    //       This means that it must rely on some global state that can be accessed by all browsing contexts, which is the main thread event loop.
    HTML::main_thread_event_loop().push_onto_backup_incumbent_settings_object_stack({}, *this);

    // 2. Let context be the topmost script-having execution context.
    auto* context = top_most_script_having_execution_context(vm);

    // 3. If context is not null, increment context's skip-when-determining-incumbent counter.
    if (context)
        context->skip_when_determining_incumbent_counter++;
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#parse-a-url
AK::URL EnvironmentSettingsObject::parse_url(StringView url)
{
    // 1. Let encoding be document's character encoding, if document was given, and environment settings object's API URL character encoding otherwise.
    // FIXME: Pass in environment settings object's API URL character encoding.

    // 2. Let baseURL be document's base URL, if document was given, and environment settings object's API base URL otherwise.
    auto base_url = api_base_url();

    // 3. Let urlRecord be the result of applying the URL parser to url, with baseURL and encoding.
    // 4. If urlRecord is failure, then return failure.
    // 5. Let urlString be the result of applying the URL serializer to urlRecord.
    // 6. Return urlString as the resulting URL string and urlRecord as the resulting URL record.
    return base_url.complete_url(url);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#clean-up-after-running-a-callback
void EnvironmentSettingsObject::clean_up_after_running_callback()
{
    auto& vm = global_object().vm();

    // 1. Let context be the topmost script-having execution context.
    auto* context = top_most_script_having_execution_context(vm);

    // 2. If context is not null, decrement context's skip-when-determining-incumbent counter.
    if (context)
        context->skip_when_determining_incumbent_counter--;

    // 3. Assert: the topmost entry of the backup incumbent settings object stack is settings.
    auto& event_loop = HTML::main_thread_event_loop();
    VERIFY(&event_loop.top_of_backup_incumbent_settings_object_stack() == this);

    // 4. Remove settings from the backup incumbent settings object stack.
    event_loop.pop_backup_incumbent_settings_object_stack({});
}

void EnvironmentSettingsObject::push_onto_outstanding_rejected_promises_weak_set(JS::Promise* promise)
{
    m_outstanding_rejected_promises_weak_set.append(promise);
}

bool EnvironmentSettingsObject::remove_from_outstanding_rejected_promises_weak_set(JS::Promise* promise)
{
    return m_outstanding_rejected_promises_weak_set.remove_first_matching([&](JS::Promise* promise_in_set) {
        return promise == promise_in_set;
    });
}

void EnvironmentSettingsObject::push_onto_about_to_be_notified_rejected_promises_list(JS::NonnullGCPtr<JS::Promise> promise)
{
    m_about_to_be_notified_rejected_promises_list.append(JS::make_handle(promise));
}

bool EnvironmentSettingsObject::remove_from_about_to_be_notified_rejected_promises_list(JS::NonnullGCPtr<JS::Promise> promise)
{
    return m_about_to_be_notified_rejected_promises_list.remove_first_matching([&](auto& promise_in_list) {
        return promise == promise_in_list;
    });
}

// https://html.spec.whatwg.org/multipage/webappapis.html#notify-about-rejected-promises
void EnvironmentSettingsObject::notify_about_rejected_promises(Badge<EventLoop>)
{
    // 1. Let list be a copy of settings object's about-to-be-notified rejected promises list.
    auto list = m_about_to_be_notified_rejected_promises_list;

    // 2. If list is empty, return.
    if (list.is_empty())
        return;

    // 3. Clear settings object's about-to-be-notified rejected promises list.
    m_about_to_be_notified_rejected_promises_list.clear();

    // 4. Let global be settings object's global object.
    auto& global = global_object();

    // 5. Queue a global task on the DOM manipulation task source given global to run the following substep:
    queue_global_task(Task::Source::DOMManipulation, global, [this, &global, list = move(list)] {
        // 1. For each promise p in list:
        for (auto promise : list) {

            // 1. If p's [[PromiseIsHandled]] internal slot is true, continue to the next iteration of the loop.
            if (promise->is_handled())
                continue;

            // 2. Let notHandled be the result of firing an event named unhandledrejection at global, using PromiseRejectionEvent, with the cancelable attribute initialized to true,
            //    the promise attribute initialized to p, and the reason attribute initialized to the value of p's [[PromiseResult]] internal slot.
            PromiseRejectionEventInit event_init {
                {
                    .bubbles = false,
                    .cancelable = true,
                    .composed = false,
                },
                // Sadly we can't use .promise and .reason here, as we can't use the designator on the initialization of DOM::EventInit above.
                /* .promise = */ JS::make_handle(*promise),
                /* .reason = */ promise->result(),
            };
            // FIXME: This currently assumes that global is a WindowObject.
            auto& window = verify_cast<HTML::Window>(global);

            auto promise_rejection_event = PromiseRejectionEvent::create(window.realm(), HTML::EventNames::unhandledrejection, event_init);

            bool not_handled = window.dispatch_event(*promise_rejection_event);

            // 3. If notHandled is false, then the promise rejection is handled. Otherwise, the promise rejection is not handled.

            // 4. If p's [[PromiseIsHandled]] internal slot is false, add p to settings object's outstanding rejected promises weak set.
            if (!promise->is_handled())
                m_outstanding_rejected_promises_weak_set.append(*promise);

            // This algorithm results in promise rejections being marked as handled or not handled. These concepts parallel handled and not handled script errors.
            // If a rejection is still not handled after this, then the rejection may be reported to a developer console.
            if (not_handled)
                HTML::report_exception_to_console(promise->result(), realm(), ErrorInPromise::Yes);
        }
    });
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-script
bool EnvironmentSettingsObject::is_scripting_enabled() const
{
    // Scripting is enabled for an environment settings object settings when all of the following conditions are true:
    // The user agent supports scripting.
    // NOTE: This is always true in LibWeb :^)

    // FIXME: Do the right thing for workers.
    if (!is<HTML::Window>(m_realm_execution_context->realm->global_object()))
        return true;

    // The user has not disabled scripting for settings at this time. (User agents may provide users with the option to disable scripting globally, or in a finer-grained manner, e.g., on a per-origin basis, down to the level of individual environment settings objects.)
    auto document = const_cast<EnvironmentSettingsObject&>(*this).responsible_document();
    VERIFY(document);
    if (!document->page().is_scripting_enabled())
        return false;

    // FIXME: Either settings's global object is not a Window object, or settings's global object's associated Document's active sandboxing flag set does not have its sandboxed scripts browsing context flag set.

    return true;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-noscript
bool EnvironmentSettingsObject::is_scripting_disabled() const
{
    // Scripting is disabled for an environment settings object when scripting is not enabled for it, i.e., when any of the above conditions are false.
    return !is_scripting_enabled();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#module-type-allowed
bool EnvironmentSettingsObject::module_type_allowed(StringView module_type) const
{
    // 1. If moduleType is not "javascript", "css", or "json", then return false.
    if (module_type != "javascript"sv && module_type != "css"sv && module_type != "json"sv)
        return false;

    // FIXME: 2. If moduleType is "css" and the CSSStyleSheet interface is not exposed in settings's Realm, then return false.

    // 3. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#disallow-further-import-maps
void EnvironmentSettingsObject::disallow_further_import_maps()
{
    // 1. Let global be settingsObject's global object.
    auto& global = global_object();

    // 2. If global does not implement Window, then return.
    if (!is<Window>(global))
        return;

    // 3. Set global's import maps allowed to false.
    verify_cast<Window>(global).set_import_maps_allowed(false);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#incumbent-settings-object
EnvironmentSettingsObject& incumbent_settings_object()
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto& vm = event_loop.vm();

    // 1. Let context be the topmost script-having execution context.
    auto* context = top_most_script_having_execution_context(vm);

    // 2. If context is null, or if context's skip-when-determining-incumbent counter is greater than zero, then:
    if (!context || context->skip_when_determining_incumbent_counter > 0) {
        // 1. Assert: the backup incumbent settings object stack is not empty.
        // NOTE: If this assertion fails, it's because the incumbent settings object was used with no involvement of JavaScript.
        VERIFY(!event_loop.is_backup_incumbent_settings_object_stack_empty());

        // 2. Return the topmost entry of the backup incumbent settings object stack.
        return event_loop.top_of_backup_incumbent_settings_object_stack();
    }

    // 3. Return context's Realm component's settings object.
    return Bindings::host_defined_environment_settings_object(*context->realm);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-incumbent-realm
JS::Realm& incumbent_realm()
{
    // Then, the incumbent Realm is the Realm of the incumbent settings object.
    return incumbent_settings_object().realm();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-incumbent-global
JS::Object& incumbent_global_object()
{
    // Similarly, the incumbent global object is the global object of the incumbent settings object.
    return incumbent_settings_object().global_object();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#current-settings-object
EnvironmentSettingsObject& current_settings_object()
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto& vm = event_loop.vm();

    // Then, the current settings object is the environment settings object of the current Realm Record.
    return Bindings::host_defined_environment_settings_object(*vm.current_realm());
}

// https://html.spec.whatwg.org/multipage/webappapis.html#current-global-object
JS::Object& current_global_object()
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto& vm = event_loop.vm();

    // Similarly, the current global object is the global object of the current Realm Record.
    return vm.current_realm()->global_object();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-relevant-realm
JS::Realm& relevant_realm(JS::Object const& object)
{
    // The relevant Realm for a platform object is the value of its [[Realm]] field.
    return object.shape().realm();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#relevant-settings-object
EnvironmentSettingsObject& relevant_settings_object(JS::Object const& object)
{
    // Then, the relevant settings object for a platform object o is the environment settings object of the relevant Realm for o.
    return Bindings::host_defined_environment_settings_object(relevant_realm(object));
}

EnvironmentSettingsObject& relevant_settings_object(DOM::Node const& node)
{
    // Then, the relevant settings object for a platform object o is the environment settings object of the relevant Realm for o.
    return const_cast<DOM::Document&>(node.document()).relevant_settings_object();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-relevant-global
JS::Object& relevant_global_object(JS::Object const& object)
{
    // Similarly, the relevant global object for a platform object o is the global object of the relevant Realm for o.
    return relevant_realm(object).global_object();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-entry-realm
JS::Realm& entry_realm()
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto& vm = event_loop.vm();

    // With this in hand, we define the entry execution context to be the most recently pushed item in the JavaScript execution context stack that is a realm execution context.
    // The entry realm is the entry execution context's Realm component.
    // NOTE: Currently all execution contexts in LibJS are realm execution contexts
    return *vm.running_execution_context().realm;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#entry-settings-object
EnvironmentSettingsObject& entry_settings_object()
{
    // Then, the entry settings object is the environment settings object of the entry realm.
    return Bindings::host_defined_environment_settings_object(entry_realm());
}

// https://html.spec.whatwg.org/multipage/webappapis.html#entry-global-object
JS::Object& entry_global_object()
{
    // Similarly, the entry global object is the global object of the entry realm.
    return entry_realm().global_object();
}

JS::VM& relevant_agent(JS::Object const& object)
{
    // The relevant agent for a platform object platformObject is platformObject's relevant Realm's agent.
    // Spec Note: This pointer is not yet defined in the JavaScript specification; see tc39/ecma262#1357.
    return relevant_realm(object).vm();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#secure-context
bool is_secure_context(Environment const& environment)
{
    // 1. If environment is an environment settings object, then:
    if (is<EnvironmentSettingsObject>(environment)) {
        // 1. Let global be environment's global object.
        // FIXME: Add a const global_object() getter to ESO
        auto& global = static_cast<EnvironmentSettingsObject&>(const_cast<Environment&>(environment)).global_object();

        // 2. If global is a WorkerGlobalScope, then:
        if (is<WorkerGlobalScope>(global)) {
            // FIXME: 1. If global's owner set[0]'s relevant settings object is a secure context, then return true.
            // NOTE: We only need to check the 0th item since they will necessarily all be consistent.

            // 2. Return false.
            return false;
        }

        // FIXME: 3. If global is a WorkletGlobalScope, then return true.
        // NOTE: Worklets can only be created in secure contexts.
    }

    // 2. If the result of Is url potentially trustworthy? given environment's top-level creation URL is "Potentially Trustworthy", then return true.
    if (SecureContexts::is_url_potentially_trustworthy(environment.top_level_creation_url) == SecureContexts::Trustworthiness::PotentiallyTrustworthy)
        return true;

    // 3. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#non-secure-context
bool is_non_secure_context(Environment const& environment)
{
    // An environment is a non-secure context if it is not a secure context.
    return !is_secure_context(environment);
}

}
