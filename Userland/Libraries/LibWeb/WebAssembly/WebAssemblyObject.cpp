/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebAssemblyMemoryPrototype.h"
#include <AK/ScopeGuard.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWasm/AbstractMachine/Interpreter.h>
#include <LibWeb/Bindings/WindowObject.h>
#include <LibWeb/WebAssembly/WebAssemblyObject.h>

namespace Web::Bindings {

static JS::NativeFunction* create_native_function(Wasm::FunctionAddress address, String name, JS::GlobalObject& global_object);
static JS::Value to_js_value(Wasm::Value& wasm_value, JS::GlobalObject& global_object);
static Optional<Wasm::Value> to_webassembly_value(JS::Value value, const Wasm::ValueType& type, JS::GlobalObject& global_object);

WebAssemblyObject::WebAssemblyObject(JS::GlobalObject& global_object)
    : Object(*global_object.object_prototype())
{
}

void WebAssemblyObject::initialize(JS::GlobalObject& global_object)
{
    Object::initialize(global_object);

    define_native_function("validate", validate, 1);
    define_native_function("compile", compile, 1);
    define_native_function("instantiate", instantiate, 1);

    auto& vm = global_object.vm();

    auto& window = static_cast<WindowObject&>(global_object);
    auto& memory_constructor = window.ensure_web_prototype<WebAssemblyMemoryConstructor>("WebAssembly.Memory");
    memory_constructor.define_property(vm.names.name, js_string(vm, "WebAssembly.Memory"), JS::Attribute::Configurable);
    auto& memory_prototype = window.ensure_web_prototype<WebAssemblyMemoryPrototype>("WebAssemblyMemoryPrototype");
    memory_prototype.define_property(vm.names.constructor, &memory_constructor, JS::Attribute::Writable | JS::Attribute::Configurable);
    define_property("Memory", &memory_constructor);
}

NonnullOwnPtrVector<WebAssemblyObject::CompiledWebAssemblyModule> WebAssemblyObject::s_compiled_modules;
NonnullOwnPtrVector<Wasm::ModuleInstance> WebAssemblyObject::s_instantiated_modules;
Vector<WebAssemblyObject::ModuleCache> WebAssemblyObject::s_module_caches;
WebAssemblyObject::GlobalModuleCache WebAssemblyObject::s_global_cache;
Wasm::AbstractMachine WebAssemblyObject::s_abstract_machine;

void WebAssemblyObject::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);

    for (auto& entry : s_global_cache.function_instances)
        visitor.visit(entry.value);
    for (auto& module_cache : s_module_caches) {
        for (auto& entry : module_cache.function_instances)
            visitor.visit(entry.value);
        for (auto& entry : module_cache.memory_instances)
            visitor.visit(entry.value);
    }
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyObject::validate)
{
    // FIXME: Implement this once module validation is implemented in LibWasm.
    dbgln("Hit WebAssemblyObject::validate() stub!");
    return JS::Value { true };
}

static Result<size_t, JS::Value> parse_module(JS::GlobalObject& global_object, JS::Object* buffer)
{
    ByteBuffer* bytes;
    if (is<JS::ArrayBuffer>(buffer)) {
        auto array_buffer = static_cast<JS::ArrayBuffer*>(buffer);
        bytes = &array_buffer->buffer();
    } else if (is<JS::TypedArrayBase>(buffer)) {
        auto array = static_cast<JS::TypedArrayBase*>(buffer);
        bytes = &array->viewed_array_buffer()->buffer();
    } else {
        auto error = JS::TypeError::create(global_object, String::formatted("{} is not an ArrayBuffer", buffer->class_name()));
        return JS::Value { error };
    }
    InputMemoryStream stream { *bytes };
    auto module_result = Wasm::Module::parse(stream);
    ScopeGuard drain_errors {
        [&] {
            stream.handle_any_error();
        }
    };
    if (module_result.is_error()) {
        // FIXME: Throw CompileError instead.
        auto error = JS::TypeError::create(global_object, Wasm::parse_error_to_string(module_result.error()));
        return JS::Value { error };
    }

    WebAssemblyObject::s_compiled_modules.append(make<WebAssemblyObject::CompiledWebAssemblyModule>(module_result.release_value()));
    return WebAssemblyObject::s_compiled_modules.size() - 1;
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyObject::compile)
{
    // FIXME: This shouldn't block!
    auto buffer = vm.argument(0).to_object(global_object);
    JS::Value rejection_value;
    if (vm.exception()) {
        rejection_value = vm.exception()->value();
        vm.clear_exception();
    }
    auto promise = JS::Promise::create(global_object);
    if (!rejection_value.is_empty()) {
        promise->reject(rejection_value);
        return promise;
    }
    auto result = parse_module(global_object, buffer);
    if (result.is_error())
        promise->reject(result.error());
    else
        promise->fulfill(vm.heap().allocate<WebAssemblyModuleObject>(global_object, global_object, result.value()));
    return promise;
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyObject::instantiate)
{
    // FIXME: This shouldn't block!
    auto buffer = vm.argument(0).to_object(global_object);
    auto promise = JS::Promise::create(global_object);
    auto take_exception_and_reject_if_needed = [&] {
        if (vm.exception()) {
            auto rejection_value = vm.exception()->value();
            vm.clear_exception();
            promise->reject(rejection_value);
            return true;
        }

        return false;
    };

    if (take_exception_and_reject_if_needed())
        return promise;

    const Wasm::Module* module { nullptr };
    if (is<JS::ArrayBuffer>(buffer) || is<JS::TypedArrayBase>(buffer)) {
        auto result = parse_module(global_object, buffer);
        if (result.is_error()) {
            promise->reject(result.error());
            return promise;
        }
        module = &WebAssemblyObject::s_compiled_modules.at(result.value()).module;
    } else if (is<WebAssemblyModuleObject>(buffer)) {
        module = &static_cast<WebAssemblyModuleObject*>(buffer)->module();
    } else {
        auto error = JS::TypeError::create(global_object, String::formatted("{} is not an ArrayBuffer or a Module", buffer->class_name()));
        promise->reject(error);
        return promise;
    }
    VERIFY(module);

    Wasm::Linker linker { *module };
    HashMap<Wasm::Linker::Name, Wasm::ExternValue> resolved_imports;
    auto import_argument = vm.argument(1);
    if (!import_argument.is_undefined()) {
        [[maybe_unused]] auto import_object = import_argument.to_object(global_object);
        if (take_exception_and_reject_if_needed())
            return promise;

        dbgln("Trying to resolve stuff because import object was specified");
        for (const Wasm::Linker::Name& import_name : linker.unresolved_imports()) {
            dbgln("Trying to resolve {}::{}", import_name.module, import_name.name);
            auto value = import_object->get(import_name.module);
            if (vm.exception())
                break;
            auto object = value.to_object(global_object);
            if (vm.exception())
                break;

            auto import_ = object->get(import_name.name);
            if (vm.exception())
                break;
            import_name.type.visit(
                [&](Wasm::TypeIndex index) {
                    dbgln("Trying to resolve a function {}::{}, type index {}", import_name.module, import_name.name, index.value());
                    auto& type = module->type(index);
                    // FIXME: IsCallable()
                    if (!import_.is_function())
                        return;
                    auto& function = import_.as_function();
                    // FIXME: If this is a function created by create_native_function(),
                    //        just extract its address and resolve to that.
                    Wasm::HostFunction host_function {
                        [&](auto&, auto& arguments) -> Wasm::Result {
                            JS::MarkedValueList argument_values { vm.heap() };
                            for (auto& entry : arguments)
                                argument_values.append(to_js_value(entry, global_object));

                            auto result = vm.call(function, JS::js_undefined(), move(argument_values));
                            if (vm.exception()) {
                                vm.clear_exception();
                                return Wasm::Trap();
                            }
                            if (type.results().is_empty())
                                return Wasm::Result { Vector<Wasm::Value> {} };

                            if (type.results().size() == 1) {
                                auto value = to_webassembly_value(result, type.results().first(), global_object);
                                if (!value.has_value())
                                    return Wasm::Trap {};

                                return Wasm::Result { Vector<Wasm::Value> { value.release_value() } };
                            }

                            // FIXME: Multiple returns
                            TODO();
                        },
                        type
                    };
                    auto address = s_abstract_machine.store().allocate(move(host_function));
                    dbgln("Resolved to {}", address->value());
                    // FIXME: LinkError instead.
                    VERIFY(address.has_value());

                    resolved_imports.set(import_name, Wasm::ExternValue { Wasm::FunctionAddress { *address } });
                },
                [&](Wasm::GlobalType const& type) {
                    Optional<Wasm::GlobalAddress> address;
                    // https://webassembly.github.io/spec/js-api/#read-the-imports step 5.1
                    if (import_.is_number() || import_.is_bigint()) {
                        if (import_.is_number() && type.type().kind() == Wasm::ValueType::I64) {
                            // FIXME: Throw a LinkError instead.
                            vm.throw_exception<JS::TypeError>(global_object, "LinkError: Import resolution attempted to cast a Number to a BigInteger");
                            return;
                        }
                        if (import_.is_bigint() && type.type().kind() != Wasm::ValueType::I64) {
                            // FIXME: Throw a LinkError instead.
                            vm.throw_exception<JS::TypeError>(global_object, "LinkError: Import resolution attempted to cast a BigInteger to a Number");
                            return;
                        }
                        auto cast_value = to_webassembly_value(import_, type.type(), global_object);
                        if (!cast_value.has_value())
                            return;
                        address = s_abstract_machine.store().allocate({ type.type(), false }, cast_value.release_value());
                    } else {
                        // FIXME: https://webassembly.github.io/spec/js-api/#read-the-imports step 5.2
                        //        if v implements Global
                        //            let globaladdr be v.[[Global]]

                        // FIXME: Throw a LinkError instead
                        vm.throw_exception<JS::TypeError>(global_object, "LinkError: Invalid value for global type");
                        return;
                    }

                    resolved_imports.set(import_name, Wasm::ExternValue { *address });
                },
                [&](Wasm::MemoryType const&) {
                    if (!import_.is_object() || !is<WebAssemblyMemoryObject>(import_.as_object())) {
                        // FIXME: Throw a LinkError instead
                        vm.throw_exception<JS::TypeError>(global_object, "LinkError: Expected an instance of WebAssembly.Memory for a memory import");
                        return;
                    }
                    auto address = static_cast<WebAssemblyMemoryObject const&>(import_.as_object()).address();
                    resolved_imports.set(import_name, Wasm::ExternValue { address });
                },
                [&](const auto&) {
                    // FIXME: Implement these.
                    dbgln("Unimplemented import of non-function attempted");
                    vm.throw_exception<JS::TypeError>(global_object, "LinkError: Not Implemented");
                });
            if (vm.exception())
                break;
        }

        if (take_exception_and_reject_if_needed())
            return promise;
    }

    linker.link(resolved_imports);
    auto link_result = linker.finish();
    if (link_result.is_error()) {
        // FIXME: Throw a LinkError.
        StringBuilder builder;
        builder.append("LinkError: Missing ");
        builder.join(' ', link_result.error().missing_imports);
        auto error = JS::TypeError::create(global_object, builder.build());
        promise->reject(error);
        return promise;
    }

    auto instance_result = s_abstract_machine.instantiate(*module, link_result.release_value());
    if (instance_result.is_error()) {
        // FIXME: Throw a LinkError instead.
        auto error = JS::TypeError::create(global_object, instance_result.error().error);
        promise->reject(error);
        return promise;
    }

    s_instantiated_modules.append(instance_result.release_value());
    s_module_caches.empend();
    promise->fulfill(vm.heap().allocate<WebAssemblyInstanceObject>(global_object, global_object, s_instantiated_modules.size() - 1));
    return promise;
}

WebAssemblyModuleObject::WebAssemblyModuleObject(JS::GlobalObject& global_object, size_t index)
    : Object(*global_object.object_prototype())
    , m_index(index)
{
}

WebAssemblyInstanceObject::WebAssemblyInstanceObject(JS::GlobalObject& global_object, size_t index)
    : Object(static_cast<WindowObject&>(global_object).ensure_web_prototype<WebAssemblyInstancePrototype>(class_name()))
    , m_index(index)
{
}

JS::Value to_js_value(Wasm::Value& wasm_value, JS::GlobalObject& global_object)
{
    switch (wasm_value.type().kind()) {
    case Wasm::ValueType::I64:
        return global_object.heap().allocate<JS::BigInt>(global_object, Crypto::SignedBigInteger::create_from(wasm_value.to<i64>().value()));
    case Wasm::ValueType::I32:
        return JS::Value(wasm_value.to<i32>().value());
    case Wasm::ValueType::F64:
        return JS::Value(static_cast<double>(wasm_value.to<float>().value()));
    case Wasm::ValueType::F32:
        return JS::Value(wasm_value.to<double>().value());
    case Wasm::ValueType::FunctionReference:
        // FIXME: What's the name of a function reference that isn't exported?
        return create_native_function(wasm_value.to<Wasm::FunctionAddress>().value(), "FIXME_IHaveNoIdeaWhatThisShouldBeCalled", global_object);
    case Wasm::ValueType::NullFunctionReference:
        return JS::js_null();
    case Wasm::ValueType::ExternReference:
    case Wasm::ValueType::NullExternReference:
        TODO();
    }
    VERIFY_NOT_REACHED();
}

Optional<Wasm::Value> to_webassembly_value(JS::Value value, const Wasm::ValueType& type, JS::GlobalObject& global_object)
{
    static Crypto::SignedBigInteger two_64 = "1"_sbigint.shift_left(64);
    auto& vm = global_object.vm();

    switch (type.kind()) {
    case Wasm::ValueType::I64: {
        auto bigint = value.to_bigint(global_object);
        if (vm.exception())
            return {};
        auto value = bigint->big_integer().divided_by(two_64).remainder;
        VERIFY(value.trimmed_length() <= 2);
        BigEndian<i64> integer { 0 };
        value.export_data({ &integer, 2 });
        return Wasm::Value { static_cast<i64>(integer) };
    }
    case Wasm::ValueType::I32: {
        auto _i32 = value.to_i32(global_object);
        if (vm.exception())
            return {};
        return Wasm::Value { static_cast<i32>(_i32) };
    }
    case Wasm::ValueType::F64: {
        auto number = value.to_double(global_object);
        if (vm.exception())
            return {};
        return Wasm::Value { static_cast<double>(number) };
    }
    case Wasm::ValueType::F32: {
        auto number = value.to_double(global_object);
        if (vm.exception())
            return {};
        return Wasm::Value { static_cast<float>(number) };
    }
    case Wasm::ValueType::FunctionReference:
    case Wasm::ValueType::ExternReference:
    case Wasm::ValueType::NullFunctionReference:
    case Wasm::ValueType::NullExternReference:
        TODO();
    }

    VERIFY_NOT_REACHED();
}

JS::NativeFunction* create_native_function(Wasm::FunctionAddress address, String name, JS::GlobalObject& global_object)
{
    Optional<Wasm::FunctionType> type;
    WebAssemblyObject::s_abstract_machine.store().get(address)->visit([&](const auto& value) { type = value.type(); });
    if (auto entry = WebAssemblyObject::s_global_cache.function_instances.get(address); entry.has_value())
        return *entry;

    auto function = JS::NativeFunction::create(
        global_object,
        name,
        [address, type = type.release_value()](JS::VM& vm, JS::GlobalObject& global_object) -> JS::Value {
            Vector<Wasm::Value> values;
            values.ensure_capacity(type.parameters().size());

            // Grab as many values as needed and convert them.
            size_t index = 0;
            for (auto& type : type.parameters()) {
                auto result = to_webassembly_value(vm.argument(index++), type, global_object);
                if (result.has_value())
                    values.append(result.release_value());
                else
                    return {};
            }

            auto result = WebAssemblyObject::s_abstract_machine.invoke(address, move(values));
            // FIXME: Use the convoluted mapping of errors defined in the spec.
            if (result.is_trap()) {
                vm.throw_exception<JS::TypeError>(global_object, "Wasm execution trapped (WIP)");
                return {};
            }

            if (result.values().is_empty())
                return JS::js_undefined();

            if (result.values().size() == 1)
                return to_js_value(result.values().first(), global_object);

            Vector<JS::Value> result_values;
            for (auto& entry : result.values())
                result_values.append(to_js_value(entry, global_object));

            return JS::Array::create_from(global_object, result_values);
        });

    WebAssemblyObject::s_global_cache.function_instances.set(address, function);
    return function;
}

void WebAssemblyInstanceObject::initialize(JS::GlobalObject& global_object)
{
    Object::initialize(global_object);

    VERIFY(!m_exports_object);
    m_exports_object = JS::Object::create(global_object, nullptr);
    auto& instance = this->instance();
    auto& cache = this->cache();
    for (auto& export_ : instance.exports()) {
        export_.value().visit(
            [&](const Wasm::FunctionAddress& address) {
                auto object = cache.function_instances.get(address);
                if (!object.has_value()) {
                    object = create_native_function(address, export_.name(), global_object);
                    cache.function_instances.set(address, *object);
                }
                m_exports_object->define_property(export_.name(), *object);
            },
            [&](const Wasm::MemoryAddress& address) {
                auto object = cache.memory_instances.get(address);
                if (!object.has_value()) {
                    object = heap().allocate<WebAssemblyMemoryObject>(global_object, global_object, address);
                    cache.memory_instances.set(address, *object);
                }
                m_exports_object->define_property(export_.name(), *object);
            },
            [&](const auto&) {
                // FIXME: Implement other exports!
            });
    }

    m_exports_object->set_integrity_level(IntegrityLevel::Frozen);
}

void WebAssemblyInstanceObject::visit_edges(Cell::Visitor& visitor)
{
    Object::visit_edges(visitor);
    visitor.visit(m_exports_object);
}

WebAssemblyMemoryObject::WebAssemblyMemoryObject(JS::GlobalObject& global_object, Wasm::MemoryAddress address)
    : Object(static_cast<WindowObject&>(global_object).ensure_web_prototype<WebAssemblyMemoryPrototype>(class_name()))
    , m_address(address)
{
}

}
