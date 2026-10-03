;; Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
;; SPDX-License-Identifier: GPL-3.0-or-later

(local ffi (require :scripts.core.ffi_cdef))

(local CODE_TO_TYPE {:f :float
                     :d :double
                     :i :int32_t
                     :I :uint32_t
                     :Q :uint64_t
                     :EvtF :ZHLN_ContactEventF
                     :EvtD :ZHLN_ContactEventD})

(local BufferMT {})
(local TypeCache {})

(local uint32-ptr (ffi.typeof :uint32_t*))

;; Register dual metatable-free raw structure to avoid Interceptor collision
(local (ok _) (pcall ffi.typeof :ZHLN_BufferView_Raw))
(when (not ok)
  (ffi.cdef "
      typedef struct ZHLN_BufferView_Raw {
          void*    buf;
          void*    obj;
          size_t   len;
          uint32_t itemsize;
          char     format[8];
          int      readonly;
          uint32_t ndim;
          size_t   shape[4];
          size_t   strides[4];
          uint32_t flags;
          uint32_t owner_type;
      } ZHLN_BufferView_Raw;
  "))

(local RawType (ffi.typeof :ZHLN_BufferView_Raw*))

(fn get-ctype [format-ptr]
  (let [key (. (ffi.cast uint32-ptr format-ptr) 0)]
    (or (. TypeCache key) (let [fmt (ffi.string format-ptr)
                                real-type (or (. CODE_TO_TYPE fmt) :char)
                                t (ffi.typeof (.. real-type "*"))]
                            (tset TypeCache key t)
                            t))))

(fn BufferMT.get [self i j k l]
  (var offset 0)
  (when i
    (set offset (+ offset (* i (. self.strides 0)))))
  (when j
    (set offset (+ offset (* j (. self.strides 1)))))
  (when k
    (set offset (+ offset (* k (. self.strides 2)))))
  (when l
    (set offset (+ offset (* l (. self.strides 3)))))
  (let [ptr (+ (ffi.cast :char* self.buf) offset)]
    (. (ffi.cast (get-ctype self.format) ptr) 0)))

(fn BufferMT.set [self val i j k l]
  (when (not= self.readonly 0) (error "Buffer is Read-Only"))
  (var offset 0)
  (when i
    (set offset (+ offset (* i (. self.strides 0)))))
  (when j
    (set offset (+ offset (* j (. self.strides 1)))))
  (when k
    (set offset (+ offset (* k (. self.strides 2)))))
  (when l
    (set offset (+ offset (* l (. self.strides 3)))))
  (let [ptr (+ (ffi.cast :char* self.buf) offset)]
    (tset (ffi.cast (get-ctype self.format) ptr) 0 val)))

(var release-id nil)

(fn BufferMT.release [self]
  (let [raw (ffi.cast RawType self)]
    (when (not= raw.obj nil)
      (when (not release-id)
        (set release-id (ffi.C.ZHLN_GetCommandID :ReleaseBuffer)))
      (let [args (ffi.new :ReleaseBufferArgs [raw.obj])]
        (ffi.C.ZHLN_DispatchCommand nil release-id args)
        (set raw.obj nil)))))

(local COMPONENT_MAP {:x 0 :y 1 :z 2 :w 3 :r 0 :g 1 :b 2 :a 3})

(fn BufferMT.__index [self i]
  (let [method (. BufferMT i)]
    (if method
        method
        (= (type i) :string)
        (let [idx (. COMPONENT_MAP i)]
          (if idx
              (. self idx)
              (let [raw (ffi.cast RawType self)
                    (success val) (pcall (fn [] (. raw i)))]
                (if success val nil))))
        (> self.ndim 1)
        (let [sub (ffi.new :ZHLN_BufferView)]
          (set sub.obj nil)
          (set sub.itemsize self.itemsize)
          (set sub.readonly self.readonly)
          (ffi.copy sub.format self.format 8)
          (set sub.buf (+ (ffi.cast :char* self.buf) (* i (. self.strides 0))))
          (set sub.ndim (- self.ndim 1))
          (for [d 0 (- sub.ndim 1)]
            (tset sub.shape d (. self.shape (+ d 1)))
            (tset sub.strides d (. self.strides (+ d 1))))
          sub)
        (let [ptr (+ (ffi.cast :char* self.buf) (* i (. self.strides 0)))]
          (. (ffi.cast (get-ctype self.format) ptr) 0)))))

(fn BufferMT.__newindex [self i val]
  (when (not= self.readonly 0) (error "Buffer is Read-Only"))
  (if (= (type i) :string)
      (let [idx (. COMPONENT_MAP i)]
        (if idx
            (tset self idx val)
            (error (.. "Cannot assign arbitrary property '" i
                       "' to ZHLN_BufferView"))))
      (= self.ndim 1)
      (let [ptr (+ (ffi.cast :char* self.buf) (* i (. self.strides 0)))]
        (tset (ffi.cast (get-ctype self.format) ptr) 0 val))
      (> self.ndim 1)
      (let [sub (. self i)
            vtype (type val)]
        (if (= vtype :table)
            (do
              (when (not= val.x nil) (set sub.x val.x))
              (when (not= val.y nil) (set sub.y val.y))
              (when (not= val.z nil) (set sub.z val.z))
              (when (not= val.w nil) (set sub.w val.w))
              (for [k 1 (length val)]
                (when (<= k (tonumber (. sub.shape 0)))
                  (tset sub (- k 1) (. val k)))))
            (and (= vtype :cdata) (ffi.istype :ZHLN_BufferView val))
            (when (not= sub.buf val.buf)
              (let [bytes (math.min (tonumber (* (. sub.shape 0) sub.itemsize))
                                    (tonumber val.len))]
                (ffi.copy sub.buf val.buf bytes)))
            (error "Cannot assign a scalar to a high-dimensional view.")))))

(fn BufferMT.__len [self] (tonumber (. self.shape 0)))

(fn BufferMT.__gc [self] (self:release))

(pcall ffi.metatype :ZHLN_BufferView BufferMT)

{:C ffi.C}

