#include "pxtbase.h"
extern "C" void target_enable_debug();  //  Allow display of debug messages in development devices. NOTE: This will hang if no debugger is attached.
extern "C" void target_disable_debug();  //  Disable display of debug messages.  For use in production devices.
extern "C" void target_init(void);  ////  TODO
extern "C" void debug_print(const char *s);    //  TODO: Write a string to the buffered debug log.
extern "C" void debug_println(const char *s);  //  TODO: Write a string plus newline to the buffered debug log.
extern "C" void debug_flush(void);             //  TODO: Flush the buffer of the debug log so that buffered data will appear.
void debug_println(size_t l);

using namespace std;

namespace pxt {

#ifndef PXT_GC
TValue incr(TValue e) {
    if (isRefCounted(e)) {
        getVTable((RefObject *)e);
        ((RefObject *)e)->ref();
    }
    return e;
}

void decr(TValue e) {
    if (isRefCounted(e)) {
        ((RefObject *)e)->unref();
    }
}
#endif

Action mkAction(int totallen, RefAction *act) {
    check(getVTable(act)->classNo == BuiltInType::RefAction,
          PANIC_INVALID_BINARY_HEADER, 1);

    if (totallen == 0) {
        return (TValue)act; // no closure needed
    }

    void *ptr = gcAllocate(sizeof(RefAction) + totallen * sizeof(unsigned));
    RefAction *r = new (ptr) RefAction();
    r->len = totallen;
    r->func = act->func;
    memset(r->fields, 0, r->len * sizeof(unsigned));

    MEMDBG("mkAction: start=%p => %p", act, r);

    return (Action)r;
}

RefRecord *mkClassInstance(VTable *vtable) {
    intcheck(vtable->methods[0] == &RefRecord_destroy, PANIC_SIZE, 3);
    //intcheck(vtable->methods[1] == &RefRecord_print, PANIC_SIZE, 4);

    void *ptr = gcAllocate(vtable->numbytes);
    RefRecord *r = new (ptr) RefRecord(vtable);
    memset(r->fields, 0, vtable->numbytes - sizeof(RefRecord));
    MEMDBG("mkClass: vt=%p => %p", vtable, r);
    return r;
}

TValue RefRecord::ld(int idx) {
    // intcheck((reflen == 255 ? 0 : reflen) <= idx && idx < len, PANIC_OUT_OF_BOUNDS, 1);
    return fields[idx];
}

TValue RefRecord::ldref(int idx) {
    // DMESG("LD %p len=%d reflen=%d idx=%d", this, len, reflen, idx);
    // intcheck(0 <= idx && idx < reflen, PANIC_OUT_OF_BOUNDS, 2);
    TValue tmp = fields[idx];
    incr(tmp);
    return tmp;
}

void RefRecord::st(int idx, TValue v) {
    // intcheck((reflen == 255 ? 0 : reflen) <= idx && idx < len, PANIC_OUT_OF_BOUNDS, 3);
    fields[idx] = v;
}

void RefRecord::stref(int idx, TValue v) {
    // DMESG("ST %p len=%d reflen=%d idx=%d", this, len, reflen, idx);
    // intcheck(0 <= idx && idx < reflen, PANIC_OUT_OF_BOUNDS, 4);
    decr(fields[idx]);
    fields[idx] = v;
}

void RefObject::destroyVT() {
    ((RefObjectMethod)getVTable(this)->methods[0])(this);
#ifndef PXT_GC
    free(this);
#endif
}

//%
void deleteRefObject(RefObject *obj) {
    obj->destroyVT();
}

void RefObject::printVT() {
    ((RefObjectMethod)getVTable(this)->methods[1])(this);
}

void RefRecord_destroy(RefRecord *r) {
#ifndef PXT_GC
    VTable *tbl = getVTable(r);
    int len = (tbl->numbytes - sizeof(RefRecord)) >> 2;
    for (int i = 0; i < len; ++i) {
        decr(r->fields[i]);
        r->fields[i] = 0;
    }
#endif
}

void RefRecord_print(RefRecord *r) {
    DMESG("RefRecord %p r=%d size=%d bytes", r, REFCNT(r), getVTable(r)->numbytes);
}

TValue Segment::get(unsigned i) {
#ifdef DEBUG_BUILD
    DMESG("In Segment::get index:%d", i);
    this->print();
#endif

    if (i < length) {
        return data[i];
    }
    return Segment::DefaultValue;
}

void Segment::setRef(unsigned i, TValue value) {
    decr(get(i));
    set(i, value);
}

void Segment::set(unsigned i, TValue value) {
    if (i < size) {
        data[i] = value;
    } else if (i < Segment::MaxSize) {
        growByMin(i + 1);
        data[i] = value;
    } else {
        return;
    }
    if (length <= i) {
        length = i + 1;
    }

#ifdef DEBUG_BUILD
    DMESG("In Segment::set");
    this->print();
#endif

    return;
}

ramint_t Segment::growthFactor(ramint_t size) {
    if (size == 0) {
        return 4;
    }
    if (size < 64) {
        return size * 2; // Double
    }
    if (size < 512) {
        return size * 5 / 3; // Grow by 1.66 rate
    }
    // Grow by constant rate
    if ((unsigned)size + 256 < MaxSize)
        return size + 256;
    else
        return MaxSize;
}

void Segment::growByMin(ramint_t minSize) {
    growBy(max(minSize, growthFactor(size)));
}

void Segment::growBy(ramint_t newSize) {
#ifdef DEBUG_BUILD
    DMESG("growBy: %d", newSize);
    this->print();
#endif
    if (size < newSize) {
        // this will throw if unable to allocate
        TValue *tmp = (TValue *)(xmalloc(newSize * sizeof(TValue)));

        // Copy existing data
        if (size) {
            memcpy(tmp, data, size * sizeof(TValue));
        }
        // fill the rest with default value
        memset(tmp + size, 0, (newSize - size) * sizeof(TValue));

        // free older segment;
        free(data);

        data = tmp;
        size = newSize;

#ifdef DEBUG_BUILD
        DMESG("growBy - after reallocation");
        this->print();
#endif
    }
    // else { no shrinking yet; }
    return;
}

void Segment::ensure(ramint_t newSize) {
    if (newSize < size) {
        return;
    }
    growByMin(newSize);
}

void Segment::setLength(unsigned newLength) {
    if (newLength > size) {
        ensure(length);
    }
    length = newLength;
    return;
}

void Segment::push(TValue value) {
    this->set(length, value);
}

TValue Segment::pop() {
#ifdef DEBUG_BUILD
    DMESG("In Segment::pop");
    this->print();
#endif

    if (length > 0) {
        --length;
        TValue value = data[length];
        data[length] = Segment::DefaultValue;
        return value;
    }
    return Segment::DefaultValue;
}

// this function removes an element at index i and shifts the rest of the elements to
// left to fill the gap
TValue Segment::remove(unsigned i) {
#ifdef DEBUG_BUILD
    DMESG("In Segment::remove index:%d", i);
    this->print();
#endif
    if (i < length) {
        // value to return
        TValue ret = data[i];
        if (i + 1 < length) {
            // Move the rest of the elements to fill in the gap.
            memmove(data + i, data + i + 1, (length - i - 1) * sizeof(unsigned));
        }
        length--;
        data[length] = Segment::DefaultValue;
#ifdef DEBUG_BUILD
        DMESG("After Segment::remove index:%d", i);
        this->print();
#endif
        return ret;
    }
    return Segment::DefaultValue;
}

// this function inserts element value at index i by shifting the rest of the elements right.
void Segment::insert(unsigned i, TValue value) {
#ifdef DEBUG_BUILD
    DMESG("In Segment::insert index:%d value:%d", i, value);
    this->print();
#endif

    if (i < length) {
        ensure(length + 1);

        // Move the rest of the elements to fill in the gap.
        memmove(data + i + 1, data + i, (length - i) * sizeof(unsigned));

        data[i] = value;
        length++;
    } else {
        // This is insert beyond the length, just call set which will adjust the length
        set(i, value);
    }
#ifdef DEBUG_BUILD
    DMESG("After Segment::insert index:%d", i);
    this->print();
#endif
}

void Segment::print() {
    DMESG("Segment: %p, length: %d, size: %d", data, (unsigned)length, (unsigned)size);
    for (unsigned i = 0; i < size; i++) {
        DMESG("-> %d", (unsigned)(uintptr_t)data[i]);
    }
}

bool Segment::isValidIndex(unsigned i) {
    if (i > length) {
        return false;
    }
    return true;
}

void Segment::destroy() {
#ifdef DEBUG_BUILD
    DMESG("In Segment::destroy");
    this->print();
#endif
    length = size = 0;
    free(data);
    data = nullptr;
}

void RefCollection::push(TValue x) {
    incr(x);
    head.push(x);
}

TValue RefCollection::pop() {
    TValue ret = head.pop();
    incr(ret);
    return ret;
}

TValue RefCollection::getAt(int i) {
    TValue tmp = head.get(i);
    incr(tmp);
    return tmp;
}

TValue RefCollection::removeAt(int i) {
    return head.remove(i);
}

void RefCollection::insertAt(int i, TValue value) {
    head.insert(i, value);
    incr(value);
}

void RefCollection::setAt(int i, TValue value) {
    incr(value);
    head.setRef(i, value);
}

int RefCollection::indexOf(TValue x, int start) {
#ifndef X86_64
    unsigned i = start;
    while (head.isValidIndex(i)) {
        if (pxt::eq_bool(head.get(i), x)) {
            return (int)i;
        }
        i++;
    }
#endif
    return -1;
}

bool RefCollection::removeElement(TValue x) {
    int idx = indexOf(x, 0);
    if (idx >= 0) {
        decr(removeAt(idx));
        return 1;
    }
    return 0;
}

PXT_VTABLE_CTOR(RefCollection) {}

void RefCollection::destroy(RefCollection *t) {
#ifndef PXT_GC
    auto data = t->head.getData();
    auto len = t->head.getLength();
    for (unsigned i = 0; i < len; i++) {
        decr(data[i]);
    }
#endif
    t->head.destroy();
}

void RefCollection::print(RefCollection *t) {
    DMESG("RefCollection %p r=%d size=%d", t, REFCNT(t), t->head.getLength());
    t->head.print();
}

PXT_VTABLE_CTOR(RefAction) {}

// fields[] contain captured locals
void RefAction::destroy(RefAction *t) {
#ifndef PXT_GC
    for (int i = 0; i < t->len; ++i) {
        decr(t->fields[i]);
        t->fields[i] = 0;
    }
#endif
}

void RefAction::print(RefAction *t) {
    DMESG("RefAction %p r=%d pc=%X size=%d", t, REFCNT(t),
          (const uint8_t *)t->func - (const uint8_t *)bytecode, t->len);
}

PXT_VTABLE_CTOR(RefRefLocal) {
    v = 0;
}

void RefRefLocal::print(RefRefLocal *t) {
    DMESG("RefRefLocal %p r=%d v=%p", t, REFCNT(t), (void *)t->v);
}

void RefRefLocal::destroy(RefRefLocal *t) {
    decr(t->v);
}

PXT_VTABLE_CTOR(RefMap) {}

void RefMap::destroy(RefMap *t) {
#ifndef PXT_GC
    auto len = t->values.getLength();
    auto values = t->values.getData();
    auto keys = t->keys.getData();
    intcheck(t->keys.getLength() == len, PANIC_SIZE, 101);
    for (unsigned i = 0; i < len; ++i) {
        decr(values[i]);
        values[i] = nullptr;
        decr(keys[i]);
        keys[i] = nullptr;
    }
#endif
    t->keys.destroy();
    t->values.destroy();
}

int RefMap::findIdx(String key) {
    auto len = keys.getLength();
    auto data = (String *)keys.getData();

    // fast path
    for (unsigned i = 0; i < len; ++i) {
        if (data[i] == key)
            return i;
    }

    // slow path
    auto keylen = key->length;
    for (unsigned i = 0; i < len; ++i) {
        auto s = data[i];
        if (s->length == keylen && memcmp(s->data, key->data, keylen) == 0)
            return i;
    }

    return -1;
}

void RefMap::print(RefMap *t) {
    DMESG("RefMap %p r=%d size=%d", t, REFCNT(t), t->keys.getLength());
}

void debugMemLeaks() {}

void error(PXT_PANIC code, int subcode) {
    DMESG("Error: %d [%d]", code, subcode);
    target_panic(code);
}

uint16_t *bytecode;
TValue *globals;

unsigned *allocate(ramint_t sz) {
    unsigned *arr = new unsigned[sz];
    memset(arr, 0, sz * sizeof(unsigned));
    return arr;
}

void checkStr(bool cond, const char *msg) {
    if (!cond) {
        debug_print("***** Check Failed: "); debug_println(msg); debug_flush();  //// TODO
        while (true) {
        }
    }
}

int templateHash() {
    return ((int *)bytecode)[4];
}

int programHash() {
    return ((int *)bytecode)[6];
}

int getNumGlobals() {
    return bytecode[16];
}

#ifndef X86_64
void exec_binary(unsigned *pc) {
    // XXX re-enable once the calibration code is fixed and [editor/embedded.ts]
    // properly prepends a call to [internal_main].
    // ::touch_develop::internal_main();

    // unique group for radio based on source hash
    // ::touch_develop::micro_bit::radioDefaultGroup = programHash();

    unsigned ver = *pc++;
    debug_print("---exec_binary runtime "); debug_println((size_t) ver); debug_flush();  //// TODO
    checkStr(ver == 0x4210, ":( Bad runtime version");
    debug_println("---exec_binary allocate"); debug_flush();  //// TODO

    bytecode = *((uint16_t **)pc++); // the actual bytecode is here
    globals = (TValue *)allocate(getNumGlobals());
    debug_println("---exec_binary compare"); debug_flush();  //// TODO

    // can be any valid address, best in RAM for speed
    globals[0] = (TValue)&globals;

    // just compare the first word
    // TODO
    checkStr(((uint32_t *)bytecode)[0] == 0x923B8E70 && (unsigned)templateHash() == *pc,
             ":( Failed partial flash");
    debug_println("---exec_binary initPerfCounters"); debug_flush();  //// TODO

    uintptr_t startptr = (uintptr_t)bytecode;

    startptr += 64; // header

    initPerfCounters();
    debug_println("---exec_binary initRuntime"); debug_flush();  //// TODO

    initRuntime();
    debug_println("---exec_binary runAction0"); debug_flush();  //// TODO

    runAction0((Action)startptr);
    debug_println("---exec_binary releaseFiber"); debug_flush();  //// TODO

    pxt::releaseFiber();
}

void start() {
    target_enable_debug();  //// TODO
    target_init(); //// TODO
    debug_println("---pxt::start"); debug_flush();  //// TODO
    exec_binary((unsigned *)functionsAndBytecode);
}
#endif

} // namespace pxt

namespace Array_ {
//%
bool isArray(TValue arr) {
    auto vt = getAnyVTable(arr);
    return vt && vt->classNo == BuiltInType::RefCollection;
}
} // namespace Array_

namespace pxtrt {
//%
RefCollection *keysOf(TValue v) {
    auto r = NEW_GC(RefCollection);
    MEMDBG("mkColl[keys]: => %p", r);
    if (getAnyVTable(v) != &RefMap_vtable)
        return r;
    auto rm = (RefMap *)v;
    auto len = rm->keys.getLength();
    if (!len)
        return r;
    r->setLength(len);
    auto dst = r->getData();
    memcpy(dst, rm->keys.getData(), len * sizeof(TValue));
    for (unsigned i = 0; i < len; ++i)
        incr(dst[i]);
    return r;
}
} // namespace pxtrt
