/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/LegacyPlatformObject.h>
#include <LibWeb/Bindings/WindowPrototype.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebIDL {

// https://webidl.spec.whatwg.org/#dfn-get-buffer-source-copy
ErrorOr<ByteBuffer> get_buffer_source_copy(JS::Object const& buffer_source)
{
    // 1. Let esBufferSource be the result of converting bufferSource to an ECMAScript value.

    // 2. Let esArrayBuffer be esBufferSource.
    JS::ArrayBuffer* es_array_buffer;

    // 3. Let offset be 0.
    u32 offset = 0;

    // 4. Let length be 0.
    u32 length = 0;

    // 5. If esBufferSource has a [[ViewedArrayBuffer]] internal slot, then:
    if (is<JS::TypedArrayBase>(buffer_source)) {
        auto const& es_buffer_source = static_cast<JS::TypedArrayBase const&>(buffer_source);

        auto typed_array_record = JS::make_typed_array_with_buffer_witness_record(es_buffer_source, JS::ArrayBuffer::Order::SeqCst);

        // AD-HOC: The WebIDL spec has not been updated for resizable ArrayBuffer objects. This check follows the behavior of step 7.
        if (JS::is_typed_array_out_of_bounds(typed_array_record))
            return ByteBuffer {};

        // 1. Set esArrayBuffer to esBufferSource.[[ViewedArrayBuffer]].
        es_array_buffer = es_buffer_source.viewed_array_buffer();

        // 2. Set offset to esBufferSource.[[ByteOffset]].
        offset = es_buffer_source.byte_offset();

        // 3. Set length to esBufferSource.[[ByteLength]].
        length = JS::typed_array_byte_length(typed_array_record);
    } else if (is<JS::DataView>(buffer_source)) {
        auto const& es_buffer_source = static_cast<JS::DataView const&>(buffer_source);

        auto view_record = JS::make_data_view_with_buffer_witness_record(es_buffer_source, JS::ArrayBuffer::Order::SeqCst);

        // AD-HOC: The WebIDL spec has not been updated for resizable ArrayBuffer objects. This check follows the behavior of step 7.
        if (JS::is_view_out_of_bounds(view_record))
            return ByteBuffer {};

        // 1. Set esArrayBuffer to esBufferSource.[[ViewedArrayBuffer]].
        es_array_buffer = es_buffer_source.viewed_array_buffer();

        // 2. Set offset to esBufferSource.[[ByteOffset]].
        offset = es_buffer_source.byte_offset();

        // 3. Set length to esBufferSource.[[ByteLength]].
        length = JS::get_view_byte_length(view_record);
    }
    // 6. Otherwise:
    else {
        // 1. Assert: esBufferSource is an ArrayBuffer or SharedArrayBuffer object.
        auto const& es_buffer_source = static_cast<JS::ArrayBuffer const&>(buffer_source);
        es_array_buffer = &const_cast<JS::ArrayBuffer&>(es_buffer_source);

        // 2. Set length to esBufferSource.[[ArrayBufferByteLength]].
        length = es_buffer_source.byte_length();
    }

    // 7. If ! IsDetachedBuffer(esArrayBuffer) is true, then return the empty byte sequence.
    if (es_array_buffer->is_detached())
        return ByteBuffer {};

    // 8. Let bytes be a new byte sequence of length equal to length.
    auto bytes = TRY(ByteBuffer::create_zeroed(length));

    // 9. For i in the range offset to offset + length − 1, inclusive, set bytes[i − offset] to ! GetValueFromBuffer(esArrayBuffer, i, Uint8, true, Unordered).
    for (u64 i = offset; i < offset + length; ++i) {
        auto value = es_array_buffer->get_value<u8>(i, true, JS::ArrayBuffer::Unordered);
        bytes[i - offset] = static_cast<u8>(value.as_double());
    }

    // 10. Return bytes.
    return bytes;
}

// https://webidl.spec.whatwg.org/#call-user-object-operation-return
inline JS::Completion clean_up_on_return(HTML::EnvironmentSettingsObject& stored_settings, HTML::EnvironmentSettingsObject& relevant_settings, JS::Completion& completion, OperationReturnsPromise operation_returns_promise)
{
    auto& realm = stored_settings.realm();

    // Return: at this point completion will be set to an ECMAScript completion value.

    // 1. Clean up after running a callback with stored settings.
    stored_settings.clean_up_after_running_callback();

    // 2. Clean up after running script with relevant settings.
    relevant_settings.clean_up_after_running_script();

    // 3. If completion is a normal completion, return completion.
    if (completion.type() == JS::Completion::Type::Normal)
        return completion;

    // 4. If completion is an abrupt completion and the operation has a return type that is not a promise type, return completion.
    if (completion.is_abrupt() && operation_returns_promise == OperationReturnsPromise::No)
        return completion;

    // 5. Let rejectedPromise be ! Call(%Promise.reject%, %Promise%, «completion.[[Value]]»).
    auto rejected_promise = create_rejected_promise(realm, *completion.release_value());

    // 6. Return the result of converting rejectedPromise to the operation’s return type.
    // Note: The operation must return a promise, so no conversion is necessary
    return JS::Value { rejected_promise->promise() };
}

JS::Completion call_user_object_operation(WebIDL::CallbackType& callback, String const& operation_name, Optional<JS::Value> this_argument, JS::MarkedVector<JS::Value> args)
{
    // 1. Let completion be an uninitialized variable.
    JS::Completion completion;

    // 2. If thisArg was not given, let thisArg be undefined.
    if (!this_argument.has_value())
        this_argument = JS::js_undefined();

    // 3. Let O be the ECMAScript object corresponding to value.
    auto& object = callback.callback;

    // 4. Let realm be O’s associated Realm.
    auto& realm = object->shape().realm();

    // 5. Let relevant settings be realm’s settings object.
    auto& relevant_settings = Bindings::host_defined_environment_settings_object(realm);

    // 6. Let stored settings be value’s callback context.
    auto& stored_settings = callback.callback_context;

    // 7. Prepare to run script with relevant settings.
    relevant_settings.prepare_to_run_script();

    // 8. Prepare to run a callback with stored settings.
    stored_settings->prepare_to_run_callback();

    // 9. Let X be O.
    auto actual_function_object = object;

    // 10. If ! IsCallable(O) is false, then:
    if (!object->is_function()) {
        // 1. Let getResult be Get(O, opName).
        auto get_result = object->get(operation_name.to_byte_string());

        // 2. If getResult is an abrupt completion, set completion to getResult and jump to the step labeled return.
        if (get_result.is_throw_completion()) {
            completion = get_result.throw_completion();
            return clean_up_on_return(stored_settings, relevant_settings, completion, callback.operation_returns_promise);
        }

        // 4. If ! IsCallable(X) is false, then set completion to a new Completion{[[Type]]: throw, [[Value]]: a newly created TypeError object, [[Target]]: empty}, and jump to the step labeled return.
        if (!get_result.value().is_function()) {
            completion = realm.vm().template throw_completion<JS::TypeError>(JS::ErrorType::NotAFunction, get_result.value().to_string_without_side_effects());
            return clean_up_on_return(stored_settings, relevant_settings, completion, callback.operation_returns_promise);
        }

        // 3. Set X to getResult.[[Value]].
        // NOTE: This is done out of order because `actual_function_object` is of type JS::Object and we cannot assign to it until we know for sure getResult.[[Value]] is a JS::Object.
        actual_function_object = get_result.release_value().as_object();

        // 5. Set thisArg to O (overriding the provided value).
        this_argument = object;
    }

    // FIXME: 11. Let esArgs be the result of converting args to an ECMAScript arguments list. If this throws an exception, set completion to the completion value representing the thrown exception and jump to the step labeled return.
    //        For simplicity, we currently make the caller do this. However, this means we can't throw exceptions at this point like the spec wants us to.

    // 12. Let callResult be Call(X, thisArg, esArgs).
    VERIFY(actual_function_object);
    auto& vm = object->vm();
    auto call_result = JS::call(vm, verify_cast<JS::FunctionObject>(*actual_function_object), this_argument.value(), args.span());

    // 13. If callResult is an abrupt completion, set completion to callResult and jump to the step labeled return.
    if (call_result.is_throw_completion()) {
        completion = call_result.throw_completion();
        return clean_up_on_return(stored_settings, relevant_settings, completion, callback.operation_returns_promise);
    }

    // 14. Set completion to the result of converting callResult.[[Value]] to an IDL value of the same type as the operation’s return type.
    // FIXME: This does no conversion.
    completion = call_result.value();

    return clean_up_on_return(stored_settings, relevant_settings, completion, callback.operation_returns_promise);
}

// https://webidl.spec.whatwg.org/#invoke-a-callback-function
JS::Completion invoke_callback(WebIDL::CallbackType& callback, Optional<JS::Value> this_argument, JS::MarkedVector<JS::Value> args)
{
    // 1. Let completion be an uninitialized variable.
    JS::Completion completion;

    // 2. If thisArg was not given, let thisArg be undefined.
    if (!this_argument.has_value())
        this_argument = JS::js_undefined();

    // 3. Let F be the ECMAScript object corresponding to callable.
    auto& function_object = callback.callback;

    // 4. If ! IsCallable(F) is false:
    if (!function_object->is_function()) {
        // 1. Note: This is only possible when the callback function came from an attribute marked with [LegacyTreatNonObjectAsNull].

        // 2. Return the result of converting undefined to the callback function’s return type.
        // FIXME: This does no conversion.
        return { JS::js_undefined() };
    }

    // 5. Let realm be F’s associated Realm.
    // See the comment about associated realm on step 4 of call_user_object_operation.
    auto& realm = function_object->shape().realm();

    // 6. Let relevant settings be realm’s settings object.
    auto& relevant_settings = Bindings::host_defined_environment_settings_object(realm);

    // 7. Let stored settings be value’s callback context.
    auto& stored_settings = callback.callback_context;

    // 8. Prepare to run script with relevant settings.
    relevant_settings.prepare_to_run_script();

    // 9. Prepare to run a callback with stored settings.
    stored_settings->prepare_to_run_callback();

    // FIXME: 10. Let esArgs be the result of converting args to an ECMAScript arguments list. If this throws an exception, set completion to the completion value representing the thrown exception and jump to the step labeled return.
    //        For simplicity, we currently make the caller do this. However, this means we can't throw exceptions at this point like the spec wants us to.

    // 11. Let callResult be Call(F, thisArg, esArgs).
    auto& vm = function_object->vm();
    auto call_result = JS::call(vm, verify_cast<JS::FunctionObject>(*function_object), this_argument.value(), args.span());

    // 12. If callResult is an abrupt completion, set completion to callResult and jump to the step labeled return.
    if (call_result.is_throw_completion()) {
        completion = call_result.throw_completion();
        return clean_up_on_return(stored_settings, relevant_settings, completion, callback.operation_returns_promise);
    }

    // 13. Set completion to the result of converting callResult.[[Value]] to an IDL value of the same type as the operation’s return type.
    // FIXME: This does no conversion.
    completion = call_result.value();

    return clean_up_on_return(stored_settings, relevant_settings, completion, callback.operation_returns_promise);
}

JS::Completion construct(WebIDL::CallbackType& callback, JS::MarkedVector<JS::Value> args)
{
    // 1. Let completion be an uninitialized variable.
    JS::Completion completion;

    // 2. Let F be the ECMAScript object corresponding to callable.
    auto& function_object = callback.callback;

    // 4. Let realm be F’s associated Realm.
    auto& realm = function_object->shape().realm();

    // 3. If IsConstructor(F) is false, throw a TypeError exception.
    if (!JS::Value(function_object).is_constructor())
        return realm.vm().template throw_completion<JS::TypeError>(JS::ErrorType::NotAConstructor, JS::Value(function_object).to_string_without_side_effects());

    // 5. Let relevant settings be realm’s settings object.
    auto& relevant_settings = Bindings::host_defined_environment_settings_object(realm);

    // 6. Let stored settings be callable’s callback context.
    auto& stored_settings = callback.callback_context;

    // 7. Prepare to run script with relevant settings.
    relevant_settings.prepare_to_run_script();

    // 8. Prepare to run a callback with stored settings.
    stored_settings->prepare_to_run_callback();

    // FIXME: 9. Let esArgs be the result of converting args to an ECMAScript arguments list. If this throws an exception, set completion to the completion value representing the thrown exception and jump to the step labeled return.
    //        For simplicity, we currently make the caller do this. However, this means we can't throw exceptions at this point like the spec wants us to.

    // 10. Let callResult be Completion(Construct(F, esArgs)).
    auto& vm = function_object->vm();
    auto call_result = JS::construct(vm, verify_cast<JS::FunctionObject>(*function_object), args.span());

    // 11. If callResult is an abrupt completion, set completion to callResult and jump to the step labeled return.
    if (call_result.is_throw_completion()) {
        completion = call_result.throw_completion();
    }

    // 12. Set completion to the result of converting callResult.[[Value]] to an IDL value of the same type as the operation’s return type.
    else {
        // FIXME: This does no conversion.
        completion = JS::Value(call_result.value());
    }

    // 13. Return: at this point completion will be set to an ECMAScript completion value.
    // 1. Clean up after running a callback with stored settings.
    stored_settings->clean_up_after_running_callback();

    // 2. Clean up after running script with relevant settings.
    relevant_settings.clean_up_after_running_script();

    // 3. Return completion.
    return completion;
}

// https://webidl.spec.whatwg.org/#dfn-named-property-visibility
JS::ThrowCompletionOr<bool> is_named_property_exposed_on_object(Variant<Bindings::LegacyPlatformObject const*, HTML::Window*> const& variant, JS::PropertyKey const& property_key)
{
    auto const& object = *variant.visit([](auto& o) { return static_cast<JS::Object const*>(o); });

    // The spec doesn't say anything about the type of the property name here.
    // Numbers can be converted to a string, which is fine and what other engines do.
    // However, since a symbol cannot be converted to a string, it cannot be a supported property name. Return early if it's a symbol.
    if (property_key.is_symbol())
        return false;

    // 1. If P is not a supported property name of O, then return false.
    // NOTE: This is in it's own variable to enforce the type.
    auto supported_property_names = variant.visit([](auto* o) { return o->supported_property_names(); });
    auto property_key_string = MUST(String::from_byte_string(property_key.to_string()));
    if (!supported_property_names.contains_slow(property_key_string))
        return false;

    // 2. If O has an own property named P, then return false.
    // NOTE: This has to be done manually instead of using Object::has_own_property, as that would use the overridden internal_get_own_property.
    auto own_property_named_p = MUST(object.JS::Object::internal_get_own_property(property_key));

    if (own_property_named_p.has_value())
        return false;

    // 3. If O implements an interface that has the [LegacyOverrideBuiltIns] extended attribute, then return true.
    if (variant.has<Bindings::LegacyPlatformObject const*>() && variant.get<Bindings::LegacyPlatformObject const*>()->has_legacy_override_built_ins_interface_extended_attribute())
        return true;

    // 4. Let prototype be O.[[GetPrototypeOf]]().
    auto* prototype = TRY(object.internal_get_prototype_of());

    // 5. While prototype is not null:
    while (prototype) {
        // 1. If prototype is not a named properties object, and prototype has an own property named P, then return false.
        //  FIXME: Are there other named properties objects?
        if (!is<Bindings::WindowProperties>(prototype)) {
            bool prototype_has_own_property_named_p = TRY(prototype->has_own_property(property_key));
            if (prototype_has_own_property_named_p)
                return false;
        }

        // 2. Set prototype to prototype.[[GetPrototypeOf]]().
        prototype = TRY(prototype->internal_get_prototype_of());
    }

    // 6. Return true.
    return true;
}

// https://webidl.spec.whatwg.org/#abstract-opdef-integerpart
double integer_part(double n)
{
    // 1. Let r be floor(abs(n)).
    auto r = floor(abs(n));

    // 2. If n < 0, then return -1 × r.
    if (n < 0)
        return -r;

    // 3. Otherwise, return r.
    return r;
}

// https://webidl.spec.whatwg.org/#abstract-opdef-converttoint
template<Integral T>
JS::ThrowCompletionOr<T> convert_to_int(JS::VM& vm, JS::Value value, EnforceRange enforce_range, Clamp clamp)
{
    double upper_bound = 0;
    double lower_bound = 0;

    // 1. If bitLength is 64, then:
    if constexpr (sizeof(T) == 4) {
        // 1. Let upperBound be 2^(53) − 1
        upper_bound = JS::MAX_ARRAY_LIKE_INDEX;

        // 2. If signedness is "unsigned", then let lowerBound be 0.
        if constexpr (IsUnsigned<T>) {
            lower_bound = 0;
        }
        // 3. Otherwise let lowerBound be −2^(53) + 1.
        else {
            lower_bound = -JS::MAX_ARRAY_LIKE_INDEX;
        }

        // Note: this ensures long long types associated with [EnforceRange] or [Clamp] extended attributes are representable in ECMAScript’s Number type as unambiguous integers.
    } else {
        // 2. Otherwise, if signedness is "unsigned", then:
        //     1. Let lowerBound be 0.
        //     2. Let upperBound be 2^(bitLength) − 1.
        // 3. Otherwise:
        //     1. Let lowerBound be -2^(bitLength − 1).
        //     2. Let upperBound be 2^(bitLength − 1) − 1.
        lower_bound = NumericLimits<T>::min();
        upper_bound = NumericLimits<T>::max();
    }

    // 4. Let x be ? ToNumber(V).
    auto x = TRY(value.to_number(vm)).as_double();

    // 5. If x is −0, then set x to +0.
    if (x == -0.)
        x = 0.;

    // 6. If the conversion is to an IDL type associated with the [EnforceRange] extended attribute, then:
    if (enforce_range == EnforceRange::Yes) {
        // 1. If x is NaN, +∞, or −∞, then throw a TypeError.
        if (isnan(x) || isinf(x))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NumberIsNaNOrInfinity);

        // 2. Set x to IntegerPart(x).
        x = integer_part(x);

        // 3. If x < lowerBound or x > upperBound, then throw a TypeError.
        if (x < lower_bound || x > upper_bound)
            return vm.throw_completion<JS::RangeError>(MUST(String::formatted("Number '{}' is outside of allowed range of {} to {}", x, lower_bound, upper_bound)));

        // 4. Return x.
        return x;
    }

    // 7. If x is not NaN and the conversion is to an IDL type associated with the [Clamp] extended attribute, then:
    if (clamp == Clamp::Yes && !isnan(x)) {
        // 1. Set x to min(max(x, lowerBound), upperBound).
        x = min(max(x, lower_bound), upper_bound);

        // 2. Round x to the nearest integer, choosing the even integer if it lies halfway between two, and choosing +0 rather than −0.
        // 3. Return x.
        return round(x);
    }

    // 8. If x is NaN, +0, +∞, or −∞, then return +0.
    if (isnan(x) || x == 0.0 || isinf(x))
        return 0;

    // 9. Set x to IntegerPart(x).
    x = integer_part(x);

    // 10. Set x to x modulo 2^bitLength.
    auto constexpr two_pow_bitlength = NumericLimits<MakeUnsigned<T>>::max() + 1.0;
    x = JS::modulo(x, two_pow_bitlength);

    // 11. If signedness is "signed" and x ≥ 2^(bitLength − 1), then return x − 2^(bitLength).
    if (IsSigned<T> && x > NumericLimits<T>::max())
        return x - two_pow_bitlength;

    // 12. Otherwise, return x.
    return x;
}

template JS::ThrowCompletionOr<Byte> convert_to_int(JS::VM& vm, JS::Value, EnforceRange, Clamp);
template JS::ThrowCompletionOr<Octet> convert_to_int(JS::VM& vm, JS::Value, EnforceRange, Clamp);
template JS::ThrowCompletionOr<Short> convert_to_int(JS::VM& vm, JS::Value, EnforceRange, Clamp);
template JS::ThrowCompletionOr<UnsignedShort> convert_to_int(JS::VM& vm, JS::Value, EnforceRange, Clamp);
template JS::ThrowCompletionOr<Long> convert_to_int(JS::VM& vm, JS::Value, EnforceRange, Clamp);
template JS::ThrowCompletionOr<UnsignedLong> convert_to_int(JS::VM& vm, JS::Value, EnforceRange, Clamp);
template JS::ThrowCompletionOr<LongLong> convert_to_int(JS::VM& vm, JS::Value, EnforceRange, Clamp);
template JS::ThrowCompletionOr<UnsignedLongLong> convert_to_int(JS::VM& vm, JS::Value, EnforceRange, Clamp);

}
