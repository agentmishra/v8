// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/objects/js-array-buffer.h"

#include "src/base/platform/wrappers.h"
#include "src/execution/protectors-inl.h"
#include "src/logging/counters.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/property-descriptor.h"

namespace v8 {
namespace internal {

namespace {

// ES#sec-canonicalnumericindexstring
// Returns true if the lookup_key represents a valid index string.
bool CanonicalNumericIndexString(Isolate* isolate,
                                 const PropertyKey& lookup_key,
                                 bool* is_minus_zero) {
  // 1. Assert: Type(argument) is String.
  DCHECK(lookup_key.is_element() || lookup_key.name()->IsString());
  *is_minus_zero = false;
  if (lookup_key.is_element()) return true;

  Handle<String> key = Handle<String>::cast(lookup_key.name());

  // 3. Let n be ! ToNumber(argument).
  Handle<Object> result = String::ToNumber(isolate, key);
  if (result->IsMinusZero()) {
    // 2. If argument is "-0", return -0𝔽.
    // We are not performing SaveValue check for -0 because it'll be rejected
    // anyway.
    *is_minus_zero = true;
  } else {
    // 4. If SameValue(! ToString(n), argument) is false, return undefined.
    Handle<String> str = Object::ToString(isolate, result).ToHandleChecked();
    // Avoid treating strings like "2E1" and "20" as the same key.
    if (!str->SameValue(*key)) return false;
  }
  return true;
}
}  // anonymous namespace

void JSArrayBuffer::Setup(SharedFlag shared, ResizableFlag resizable,
                          std::shared_ptr<BackingStore> backing_store) {
  clear_padding();
  set_bit_field(0);
  set_is_shared(shared == SharedFlag::kShared);
  set_is_resizable(resizable == ResizableFlag::kResizable);
  set_is_detachable(shared != SharedFlag::kShared);
  for (int i = 0; i < v8::ArrayBuffer::kEmbedderFieldCount; i++) {
    SetEmbedderField(i, Smi::zero());
  }
  set_extension(nullptr);
  if (!backing_store) {
    set_backing_store(GetIsolate(), EmptyBackingStoreBuffer());
    set_byte_length(0);
    set_max_byte_length(0);
  } else {
    Attach(std::move(backing_store));
  }
  if (shared == SharedFlag::kShared) {
    GetIsolate()->CountUsage(
        v8::Isolate::UseCounterFeature::kSharedArrayBufferConstructed);
  }
}

void JSArrayBuffer::Attach(std::shared_ptr<BackingStore> backing_store) {
  DCHECK_NOT_NULL(backing_store);
  DCHECK_EQ(is_shared(), backing_store->is_shared());
  DCHECK_EQ(is_resizable(), backing_store->is_resizable());
  DCHECK_IMPLIES(
      !backing_store->is_wasm_memory() && !backing_store->is_resizable(),
      backing_store->byte_length() == backing_store->max_byte_length());
  DCHECK(!was_detached());
  Isolate* isolate = GetIsolate();

  if (backing_store->IsEmpty()) {
    set_backing_store(isolate, EmptyBackingStoreBuffer());
  } else {
    DCHECK_NE(nullptr, backing_store->buffer_start());
    set_backing_store(isolate, backing_store->buffer_start());
  }

  if (is_shared() && is_resizable()) {
    // GSABs need to read their byte_length from the BackingStore. Maintain the
    // invariant that their byte_length field is always 0.
    set_byte_length(0);
  } else {
    CHECK_LE(backing_store->byte_length(), kMaxByteLength);
    set_byte_length(backing_store->byte_length());
  }
  set_max_byte_length(backing_store->max_byte_length());
  if (backing_store->is_wasm_memory()) set_is_detachable(false);
  if (!backing_store->free_on_destruct()) set_is_external(true);
  ArrayBufferExtension* extension = EnsureExtension();
  size_t bytes = backing_store->PerIsolateAccountingLength();
  extension->set_accounting_length(bytes);
  extension->set_backing_store(std::move(backing_store));
  isolate->heap()->AppendArrayBufferExtension(*this, extension);
}

void JSArrayBuffer::Detach(bool force_for_wasm_memory) {
  if (was_detached()) return;

  if (force_for_wasm_memory) {
    // Skip the is_detachable() check.
  } else if (!is_detachable()) {
    // Not detachable, do nothing.
    return;
  }

  Isolate* const isolate = GetIsolate();
  ArrayBufferExtension* extension = this->extension();

  if (extension) {
    DisallowGarbageCollection disallow_gc;
    isolate->heap()->DetachArrayBufferExtension(*this, extension);
    std::shared_ptr<BackingStore> backing_store = RemoveExtension();
    CHECK_IMPLIES(force_for_wasm_memory, backing_store->is_wasm_memory());
  }

  if (Protectors::IsArrayBufferDetachingIntact(isolate)) {
    Protectors::InvalidateArrayBufferDetaching(isolate);
  }

  DCHECK(!is_shared());
  DCHECK(!is_asmjs_memory());
  set_backing_store(isolate, EmptyBackingStoreBuffer());
  set_byte_length(0);
  set_was_detached(true);
}

size_t JSArrayBuffer::GsabByteLength(Isolate* isolate,
                                     Address raw_array_buffer) {
  // TODO(v8:11111): Cache the last seen length in JSArrayBuffer and use it
  // in bounds checks to minimize the need for calling this function.
  DCHECK(FLAG_harmony_rab_gsab);
  DisallowGarbageCollection no_gc;
  DisallowJavascriptExecution no_js(isolate);
  JSArrayBuffer buffer = JSArrayBuffer::cast(Object(raw_array_buffer));
  CHECK(buffer.is_resizable());
  CHECK(buffer.is_shared());
  return buffer.GetBackingStore()->byte_length(std::memory_order_seq_cst);
}

ArrayBufferExtension* JSArrayBuffer::EnsureExtension() {
  ArrayBufferExtension* extension = this->extension();
  if (extension != nullptr) return extension;

  extension = new ArrayBufferExtension(std::shared_ptr<BackingStore>());
  set_extension(extension);
  return extension;
}

std::shared_ptr<BackingStore> JSArrayBuffer::RemoveExtension() {
  ArrayBufferExtension* extension = this->extension();
  DCHECK_NOT_NULL(extension);
  auto result = extension->RemoveBackingStore();
  // Remove pointer to extension such that the next GC will free it
  // automatically.
  set_extension(nullptr);
  return result;
}

void JSArrayBuffer::MarkExtension() {
  ArrayBufferExtension* extension = this->extension();
  if (extension) {
    extension->Mark();
  }
}

void JSArrayBuffer::YoungMarkExtension() {
  ArrayBufferExtension* extension = this->extension();
  if (extension) {
    extension->YoungMark();
  }
}

void JSArrayBuffer::YoungMarkExtensionPromoted() {
  ArrayBufferExtension* extension = this->extension();
  if (extension) {
    extension->YoungMarkPromoted();
  }
}

Handle<JSArrayBuffer> JSTypedArray::GetBuffer() {
  Isolate* isolate = GetIsolate();
  Handle<JSTypedArray> self(*this, isolate);
  DCHECK(IsTypedArrayOrRabGsabTypedArrayElementsKind(self->GetElementsKind()));
  Handle<JSArrayBuffer> array_buffer(JSArrayBuffer::cast(self->buffer()),
                                     isolate);
  if (!is_on_heap()) {
    // Already is off heap, so return the existing buffer.
    return array_buffer;
  }
  DCHECK(!array_buffer->is_resizable());

  // The existing array buffer should be empty.
  DCHECK(array_buffer->IsEmpty());

  // Allocate a new backing store and attach it to the existing array buffer.
  size_t byte_length = self->byte_length();
  auto backing_store =
      BackingStore::Allocate(isolate, byte_length, SharedFlag::kNotShared,
                             InitializedFlag::kUninitialized);

  if (!backing_store) {
    isolate->heap()->FatalProcessOutOfMemory("JSTypedArray::GetBuffer");
  }

  // Copy the elements into the backing store of the array buffer.
  if (byte_length > 0) {
    memcpy(backing_store->buffer_start(), self->DataPtr(), byte_length);
  }

  // Attach the backing store to the array buffer.
  array_buffer->Setup(SharedFlag::kNotShared, ResizableFlag::kNotResizable,
                      std::move(backing_store));

  // Clear the elements of the typed array.
  self->set_elements(ReadOnlyRoots(isolate).empty_byte_array());
  self->SetOffHeapDataPtr(isolate, array_buffer->backing_store(), 0);
  DCHECK(!self->is_on_heap());

  return array_buffer;
}

// ES#sec-integer-indexed-exotic-objects-defineownproperty-p-desc
// static
Maybe<bool> JSTypedArray::DefineOwnProperty(Isolate* isolate,
                                            Handle<JSTypedArray> o,
                                            Handle<Object> key,
                                            PropertyDescriptor* desc,
                                            Maybe<ShouldThrow> should_throw) {
  DCHECK(key->IsName() || key->IsNumber());
  // 1. If Type(P) is String, then
  PropertyKey lookup_key(isolate, key);
  if (lookup_key.is_element() || key->IsSmi() || key->IsString()) {
    // 1a. Let numericIndex be ! CanonicalNumericIndexString(P)
    // 1b. If numericIndex is not undefined, then
    bool is_minus_zero = false;
    if (key->IsSmi() ||  // Smi keys are definitely canonical
        CanonicalNumericIndexString(isolate, lookup_key, &is_minus_zero)) {
      // 1b i. If IsValidIntegerIndex(O, numericIndex) is false, return false.

      // IsValidIntegerIndex:
      size_t index = lookup_key.index();
      bool out_of_bounds = false;
      size_t length = o->GetLengthOrOutOfBounds(out_of_bounds);
      if (o->WasDetached() || out_of_bounds || index >= length) {
        RETURN_FAILURE(isolate, GetShouldThrow(isolate, should_throw),
                       NewTypeError(MessageTemplate::kInvalidTypedArrayIndex));
      }
      if (!lookup_key.is_element() || is_minus_zero) {
        RETURN_FAILURE(isolate, GetShouldThrow(isolate, should_throw),
                       NewTypeError(MessageTemplate::kInvalidTypedArrayIndex));
      }

      // 1b ii. If Desc has a [[Configurable]] field and if
      //     Desc.[[Configurable]] is false, return false.
      // 1b iii. If Desc has an [[Enumerable]] field and if Desc.[[Enumerable]]
      //     is false, return false.
      // 1b iv. If IsAccessorDescriptor(Desc) is true, return false.
      // 1b v. If Desc has a [[Writable]] field and if Desc.[[Writable]] is
      //     false, return false.

      if (PropertyDescriptor::IsAccessorDescriptor(desc)) {
        RETURN_FAILURE(isolate, GetShouldThrow(isolate, should_throw),
                       NewTypeError(MessageTemplate::kRedefineDisallowed, key));
      }

      if ((desc->has_configurable() && !desc->configurable()) ||
          (desc->has_enumerable() && !desc->enumerable()) ||
          (desc->has_writable() && !desc->writable())) {
        RETURN_FAILURE(isolate, GetShouldThrow(isolate, should_throw),
                       NewTypeError(MessageTemplate::kRedefineDisallowed, key));
      }

      // 1b vi. If Desc has a [[Value]] field, perform
      // ? IntegerIndexedElementSet(O, numericIndex, Desc.[[Value]]).
      if (desc->has_value()) {
        if (!desc->has_configurable()) desc->set_configurable(true);
        if (!desc->has_enumerable()) desc->set_enumerable(true);
        if (!desc->has_writable()) desc->set_writable(true);
        Handle<Object> value = desc->value();
        LookupIterator it(isolate, o, index, LookupIterator::OWN);
        RETURN_ON_EXCEPTION_VALUE(
            isolate,
            DefineOwnPropertyIgnoreAttributes(&it, value, desc->ToAttributes()),
            Nothing<bool>());
      }
      // 1b vii. Return true.
      return Just(true);
    }
  }
  // 4. Return ! OrdinaryDefineOwnProperty(O, P, Desc).
  return OrdinaryDefineOwnProperty(isolate, o, lookup_key, desc, should_throw);
}

ExternalArrayType JSTypedArray::type() {
  switch (map().elements_kind()) {
#define ELEMENTS_KIND_TO_ARRAY_TYPE(Type, type, TYPE, ctype) \
  case TYPE##_ELEMENTS:                                      \
    return kExternal##Type##Array;

    TYPED_ARRAYS(ELEMENTS_KIND_TO_ARRAY_TYPE)
    RAB_GSAB_TYPED_ARRAYS_WITH_TYPED_ARRAY_TYPE(ELEMENTS_KIND_TO_ARRAY_TYPE)
#undef ELEMENTS_KIND_TO_ARRAY_TYPE

    default:
      UNREACHABLE();
  }
}

size_t JSTypedArray::element_size() const {
  switch (map().elements_kind()) {
#define ELEMENTS_KIND_TO_ELEMENT_SIZE(Type, type, TYPE, ctype) \
  case TYPE##_ELEMENTS:                                        \
    return sizeof(ctype);

    TYPED_ARRAYS(ELEMENTS_KIND_TO_ELEMENT_SIZE)
    RAB_GSAB_TYPED_ARRAYS(ELEMENTS_KIND_TO_ELEMENT_SIZE)
#undef ELEMENTS_KIND_TO_ELEMENT_SIZE

    default:
      UNREACHABLE();
  }
}

size_t JSTypedArray::LengthTrackingGsabBackedTypedArrayLength(
    Isolate* isolate, Address raw_array) {
  // TODO(v8:11111): Cache the last seen length in JSArrayBuffer and use it
  // in bounds checks to minimize the need for calling this function.
  DCHECK(FLAG_harmony_rab_gsab);
  DisallowGarbageCollection no_gc;
  DisallowJavascriptExecution no_js(isolate);
  JSTypedArray array = JSTypedArray::cast(Object(raw_array));
  CHECK(array.is_length_tracking());
  JSArrayBuffer buffer = array.buffer();
  CHECK(buffer.is_resizable());
  CHECK(buffer.is_shared());
  size_t backing_byte_length =
      buffer.GetBackingStore()->byte_length(std::memory_order_seq_cst);
  CHECK_GE(backing_byte_length, array.byte_offset());
  auto element_byte_size = ElementsKindToByteSize(array.GetElementsKind());
  return (backing_byte_length - array.byte_offset()) / element_byte_size;
}

}  // namespace internal
}  // namespace v8
