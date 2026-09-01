/* Minimal `node:*` compatibility layer, layered the same way bootstrap.js
   layers WinterTC globals: pure spec/behavior logic lives here in JS, native
   primitives (env, cwd, exit, signal watching) are thin C wrappers installed
   by sxn_install_node_compat before this file is evaluated. The four
   `node:*` C modules (see src/node.c) just re-export the globals this file
   defines. */
(function () {
  // ---------------- events: EventEmitter ----------------
  // on/off/emit/listenerCount/listeners/removeAllListeners are native (see
  // js_ee_* in src/node.c, registered as globals above) -- phase 3 of
  // replacing node_compat.js with native C. Listener storage is still the
  // plain `this._events` object with Node's function-for-one-listener and
  // array-for-many representation; the native side reads and writes it
  // directly. `once` went native too (js_ee_once): the self-removing wrapper
  // does have a native shape after all -- a C function carrying the emitter,
  // the name and the listener, and one holder object it reads itself back
  // out of. 0.55 microseconds per once-and-emit became 0.44.
  function EventEmitter() {
    this._events = Object.create(null);
  }
  EventEmitter.prototype.on = __sxnEeOn;
  EventEmitter.prototype.addListener = __sxnEeOn;
  EventEmitter.prototype.once = __sxnEeOnce;
  EventEmitter.prototype.off = __sxnEeOff;
  EventEmitter.prototype.removeListener = __sxnEeOff;
  EventEmitter.prototype.removeAllListeners = __sxnEeRemoveAllListeners;
  EventEmitter.prototype.emit = __sxnEeEmit;
  EventEmitter.prototype.listenerCount = __sxnEeListenerCount;
  EventEmitter.prototype.listeners = __sxnEeListeners;
  // events.once(emitter, name): resolves with the emitted arguments, or
  // rejects if the emitter emits 'error' first. Widely used to await a
  // one-shot lifecycle event such as 'listening'.
  EventEmitter.once = function (emitter, name, options) {
    return new Promise((resolve, reject) => {
      const onEvent = (...args) => { cleanup(); resolve(args); };
      const onError = (err) => { cleanup(); reject(err); };
      const cleanup = () => {
        emitter.off(name, onEvent);
        if (name !== "error") emitter.off("error", onError);
        if (signal) signal.removeEventListener("abort", onAbort);
      };
      const signal = options && options.signal;
      const onAbort = () => { cleanup(); reject(signal.reason || new Error("aborted")); };
      emitter.on(name, onEvent);
      if (name !== "error") emitter.on("error", onError);
      if (signal) {
        if (signal.aborted) return onAbort();
        signal.addEventListener("abort", onAbort);
      }
    });
  };
  EventEmitter.on = function (emitter, name) {
    const queue = [], waiting = [];
    emitter.on(name, (...args) => {
      if (waiting.length) waiting.shift()({ value: args, done: false });
      else queue.push(args);
    });
    return {
      next() {
        if (queue.length) return Promise.resolve({ value: queue.shift(), done: false });
        return new Promise((res) => waiting.push(res));
      },
      [Symbol.asyncIterator]() { return this; },
    };
  };
  // Node's events module is the class with a self-reference on it, so both
  // `require('events')` and `require('events').EventEmitter` give the class.
  EventEmitter.EventEmitter = EventEmitter;
  // No `.default` here: Node does not define one, and the ESM default export
  // is set on the module itself rather than as a property of the class.
  EventEmitter.captureRejectionSymbol = Symbol.for("nodejs.rejection");
  EventEmitter.defaultMaxListeners = 10;
  globalThis.__sxnEventEmitter = EventEmitter;
  delete globalThis.__sxnEeOn;
  delete globalThis.__sxnEeOnce;
  delete globalThis.__sxnEeOff;
  delete globalThis.__sxnEeEmit;
  delete globalThis.__sxnEeListenerCount;
  delete globalThis.__sxnEeListeners;
  delete globalThis.__sxnEeRemoveAllListeners;

  // ---------------- buffer: Buffer ----------------
  // Only called for non-utf-8 encodings -- Buffer.from's string branch
  // handles utf-8 itself via a native primitive that skips the extra
  // allocation this returns-a-bare-Uint8Array shape would otherwise cost.
  // `encoding` arrives already-lowercased from that call site. The lenient
  // hex and base64 readers Node's leniency needs are native (js_hex_bytes
  // and js_base64_bytes), including the case where a string holds code
  // units above 0xff and Node's byte-at-a-time reading of it shows.
  var utf16leBytes = __sxnUtf16leBytes;
  // Native (js_buffer_decode_units in src/node.c): latin1, Node's 7-bit
  // "ascii" and utf16le are all a widening of bytes into code units, which
  // was a String.fromCharCode per byte here.
  var utf16leString = __sxnUtf16leString;

  function bufferBytesFromString(str, encoding) {
    // The native readers are lenient the way Node is -- hex stops at the
    // first pair that is not hex, base64 skips anything outside the alphabet
    // -- so ordinary payloads, including base64 with the newlines PEM and
    // MIME put in it, never touch the JavaScript loops. They hand back null
    // for a string with anything non-ASCII in it, where Node's reading of
    // UTF-16 code units is visible, and the loops below take that.
    if (encoding === "hex") {
      // Strict first, because it reads the string's own bytes with no copy
      // at all; the native lenient reader takes over when the input has
      // something in it that the strict one refuses.
      try { return Uint8Array.fromHex(str); } catch { /* fall through */ }
      return __sxnHexBytes(str);
    }
    if (encoding === "base64" || encoding === "base64url") {
      try {
        return encoding === "base64" ? Uint8Array.fromBase64(str)
                                     : Uint8Array.fromBase64(str, { alphabet: "base64url" });
      } catch { /* fall through */ }
      return __sxnBase64Bytes(str);
    }
    if (encoding === "ucs2" || encoding === "ucs-2" ||
        encoding === "utf16le" || encoding === "utf-16le") return utf16leBytes(str);
    if (encoding === "latin1" || encoding === "binary" || encoding === "ascii") return __sxnLatin1Bytes(str);
    throw new TypeError("Unknown encoding: " + encoding);
  }

  class Buffer extends Uint8Array {
    toString(encoding) {
      // Fast path: match the exact lowercase spelling (and the implicit
      // default) every real call site actually passes, with plain ===
      // comparisons -- measured ~2x faster than always paying for
      // String(encoding||"utf-8").toLowerCase()'s per-call string
      // allocation before even checking what the encoding is.
      if (encoding === undefined || encoding === "utf-8" || encoding === "utf8") return __sxnUtf8Decode(this, false, false).text; // skip a TextDecoder allocation per call
      if (encoding === "hex") return this.toHex();
      if (encoding === "base64") return this.toBase64();
      if (encoding === "base64url") return this.toBase64({ alphabet: "base64url", omitPadding: true }); // Node emits base64url unpadded
      encoding = String(encoding).toLowerCase();
      if (encoding === "utf-8" || encoding === "utf8") return __sxnUtf8Decode(this, false, false).text;
      if (encoding === "hex") return this.toHex();
      if (encoding === "base64") return this.toBase64();
      if (encoding === "base64url") return this.toBase64({ alphabet: "base64url", omitPadding: true }); // Node emits base64url unpadded
      if (encoding === "ucs2" || encoding === "ucs-2" ||
          encoding === "utf16le" || encoding === "utf-16le") return utf16leString(this);
      if (encoding === "latin1" || encoding === "binary") return __sxnLatin1String(this);
      // Node's "ascii" is 7-bit: the high bit is stripped, unlike latin1.
      if (encoding === "ascii") return __sxnAsciiString(this);
      throw new TypeError("Unknown encoding: " + encoding);
    }
    // Node's Buffer#slice (and #subarray) are zero-copy views over the same
    // backing ArrayBuffer, unlike TypedArray#slice (which copies) -- alias
    // it to #subarray to get Node's semantics instead of the inherited one.
    slice(start, end) {
      return this.subarray(start, end);
    }
    // alloc/allocUnsafe/isBuffer are installed natively after this class is
    // defined (see sxn_install_buffer_natives in src/node.c) -- pure
    // dispatch with no encoding logic, so there's nothing JS needs to do
    // here.
    static from(data, encoding) {
      if (typeof data === "string") {
        // Fast path: exact-lowercase-spelling === checks (matching every
        // real call site, including the implicit default) skip the
        // String(encoding||"utf-8").toLowerCase() allocation below entirely.
        //
        // utf-8 goes through a native primitive that hands back a bare
        // ArrayBuffer, so only one JS-visible allocation (the Buffer itself)
        // happens instead of two (a throwaway Uint8Array view plus the
        // Buffer wrapping its .buffer/.byteOffset/.byteLength). Other
        // encodings already produce a single typed array (via
        // Uint8Array.fromHex/fromBase64 or the latin1 loop); reparenting it
        // to Buffer.prototype in place is measurably faster here than
        // constructing a second Buffer instance around its .buffer.
        // Node ignores a non-string encoding rather than coercing it, so an
        // object with a toString is treated as absent and never called.
        if (typeof encoding !== "string") encoding = undefined;
        if (encoding === undefined || encoding === "utf-8" || encoding === "utf8") return new Buffer(__sxnUtf8EncodeArrayBuffer(data));
        if (encoding === "hex" || encoding === "base64" || encoding === "base64url" ||
            encoding === "ucs2" || encoding === "utf16le") {
          return Object.setPrototypeOf(bufferBytesFromString(data, encoding), Buffer.prototype);
        }
        var enc = String(encoding).toLowerCase();
        if (enc === "utf-8" || enc === "utf8") return new Buffer(__sxnUtf8EncodeArrayBuffer(data));
        return Object.setPrototypeOf(bufferBytesFromString(data, enc), Buffer.prototype);
      }
      if (data instanceof ArrayBuffer) return new Buffer(data); // a view, which is what Node gives for an ArrayBuffer
      // Native (js_buffer_from_bytes in src/node.c): copying a view went
      // through the Uint8Array subclass constructor, 0.285us against
      // 0.210us for the copy made in C. Node copies here, and code relies
      // on it -- writing to the copy must not reach the original.
      if (ArrayBuffer.isView(data)) return __sxnBufferFromBytes(data);
      // An array stays with the constructor: reading its elements one at a
      // time from C measured 1.185us against the engine's own 0.375us for
      // the same array, which it fills without leaving the interpreter.
      if (Array.isArray(data) || (data && typeof data.length === "number")) return new Buffer(data);
      throw new TypeError("Buffer.from: unsupported argument");
    }
    // Node serializes a Buffer as { type: "Buffer", data: [...] }, and code
    // round-trips through that shape.
    toJSON() { return { type: "Buffer", data: Array.from(this) }; }

    // Node orders by unsigned byte value, then by length, and equals is just
    // compare === 0. Anything holding bytes is accepted, as Node does.
    compare(other) {
      // Native (sxn_bytes_compare in src/network.c): memcmp, then length.
      return __sxnBytesCompare(this, other instanceof Uint8Array ? other : Buffer.from(other));
    }
    equals(other) { return this.length === other.length && this.compare(other) === 0; }

    // Native (js_concat_bytes in src/node.c): one memcpy per part rather
    // than a length loop, a subarray per part and a set() per part.
    static concat(list, totalLength) {
      return Object.setPrototypeOf(__sxnConcatBytes(list, totalLength), Buffer.prototype);
    }
  }
  // The numeric accessors -- readUInt32BE, writeFloatLE and the rest -- and
  // copy, all native (js_buffer_read/js_buffer_write_num/js_buffer_copy in
  // src/node.c). They are pure byte-to-number work, they are what binary
  // protocol code spends its time in, and none of them existed here before.
  Object.assign(Buffer.prototype, __sxnBufferAccessors);
  delete globalThis.__sxnBufferAccessors;

  Buffer.compare = (a, b) => __sxnBytesCompare(a, b);
  Buffer.isEncoding = (enc) =>
    ["utf8", "utf-8", "hex", "base64", "base64url", "latin1", "binary", "ascii", "ucs2", "ucs-2", "utf16le", "utf-16le"]
      .includes(String(enc).toLowerCase());
  Buffer.poolSize = 8192;

  globalThis.Buffer = Buffer;

  // ---------------- path: posix / win32 ----------------
  // Native (see js_path_posix_* in src/node.c, registered as globals above)
  // -- phase 4 of replacing node_compat.js with native C. Same
  // reduceSegments/makePathImpl algorithm, ported to walk C strings
  // instead of JS regex splits; win32 below is unchanged, still the
  // generic JS implementation.
  var posix = {
    sep: "/",
    delimiter: ":",
    isAbsolute: __sxnPosixIsAbsolute,
    normalize: __sxnPosixNormalize,
    join: __sxnPosixJoin,
    resolve: __sxnPosixResolve,
    dirname: __sxnPosixDirname,
    basename: __sxnPosixBasename,
    extname: __sxnPosixExtname,
    relative: __sxnPosixRelative,
  };

  // Native too (js_path_win_* in src/node.c). This was the last of path
  // still written in JavaScript, and the last place here that walked a
  // string with a regexp.
  var win32 = {
    sep: "\\",
    delimiter: ";",
    isAbsolute: __sxnWinIsAbsolute,
    normalize: __sxnWinNormalize,
    join: __sxnWinJoin,
    resolve: __sxnWinResolve,
    dirname: __sxnWinDirname,
    basename: __sxnWinBasename,
    extname: __sxnWinExtname,
    relative: __sxnWinRelative,
  };

  var path = __sxnIsWindows ? win32 : posix;
  path.posix = posix;
  path.win32 = win32;
  globalThis.__sxnPath = path;
  delete globalThis.__sxnPosixJoin;
  delete globalThis.__sxnPosixResolve;
  delete globalThis.__sxnPosixNormalize;
  delete globalThis.__sxnPosixIsAbsolute;
  delete globalThis.__sxnPosixDirname;
  delete globalThis.__sxnPosixBasename;
  delete globalThis.__sxnPosixExtname;
  delete globalThis.__sxnPosixRelative;
  delete globalThis.__sxnWinJoin;
  delete globalThis.__sxnWinResolve;
  delete globalThis.__sxnWinNormalize;
  delete globalThis.__sxnWinIsAbsolute;
  delete globalThis.__sxnWinDirname;
  delete globalThis.__sxnWinBasename;
  delete globalThis.__sxnWinExtname;
  delete globalThis.__sxnWinRelative;

  // ---------------- process ----------------
  function Process() {
    EventEmitter.call(this);
  }
  Process.prototype = Object.create(EventEmitter.prototype);
  Process.prototype.constructor = Process;

  var process = new Process();
  process.argv = [__sxnExecPath].concat(typeof scriptArgs !== "undefined" ? scriptArgs : []);
  // Native exotic object (see sxn_new_env_object in src/node.c) -- getenv/
  // setenv/unsetenv/environ directly behind property access, no Proxy trap
  // round-trip through JS for every read/write.
  process.env = globalThis.__sxnEnvObject;
  delete globalThis.__sxnEnvObject;
  // Node names these, and packages branch on them for path separators, line
  // endings and native-binary selection.
  process.platform = __sxnPlatform();
  process.arch = __sxnArch();
  process.version = "v" + (Sxn && Sxn.version ? Sxn.version : "0.0.0");
  process.versions = { sxn: (Sxn && Sxn.version) || "0.0.0" };
  // process.stdout / stderr: Writable enough for the logging that packages do
  // on the way past, and carrying the fd and isTTY they branch on.
  function makeStdio(fd, sink) {
    return {
      fd: fd,
      // Node leaves this undefined rather than false on a non-tty.
      isTTY: undefined,
      writable: true,
      columns: undefined,
      rows: undefined,
      write(chunk, enc, cb) {
        if (typeof enc === "function") { cb = enc; }
        sink(typeof chunk === "string" ? chunk : new TextDecoder().decode(chunk));
        if (cb) cb();
        return true;
      },
      end(chunk) { if (chunk !== undefined && chunk !== null) this.write(chunk); },
      on() { return this; },
      once() { return this; },
      off() { return this; },
      removeListener() { return this; },
      emit() { return false; },
      setEncoding() { return this; },
      cork() {}, uncork() {},
    };
  }
  // console.log appends its own newline, so the raw writes trim one to avoid
  // doubling it.
  const stripNL = (s) => (s.endsWith("\n") ? s.slice(0, -1) : s);
  process.stdout = makeStdio(1, (t) => { if (t) console.log(stripNL(t)); });
  // Straight to fd 2 -- console.error is itself built on __sxnWriteStderr, so
  // routing through it would be a cycle.
  process.stderr = makeStdio(2, (t) => { if (t) __sxnWriteStderr(t); });
  process.stdin = { fd: 0, isTTY: false, readable: false,
                    on() { return this; }, once() { return this; }, off() { return this; },
                    resume() { return this; }, pause() { return this; },
                    setEncoding() { return this; }, read() { return null; } };
  process.hrtime = Object.assign(
    function hrtime(prev) {
      const ns = BigInt(Math.round(performance.now() * 1e6));
      const s = Number(ns / 1000000000n), n = Number(ns % 1000000000n);
      if (!prev) return [s, n];
      let ds = s - prev[0], dn = n - prev[1];
      if (dn < 0) { ds -= 1; dn += 1e9; }
      return [ds, dn];
    },
    { bigint: () => BigInt(Math.round(performance.now() * 1e6)) });
  // What require() calls for a .node file. The addon's own napi_* imports
  // resolve back to this executable, so there is nothing to pass but the
  // module object it should fill in.
  if (typeof __sxnDlopen === "function") process.dlopen = __sxnDlopen;
  process.emitWarning = function (w) { console.error("Warning: " + (w && w.message ? w.message : w)); };
  process.uptime = function () { return performance.now() / 1000; };
  // The real one: a program that runs several copies of itself -- which is
  // how this runtime uses more than one core -- has nothing else to tell them
  // apart by in a log.
  process.pid = typeof __sxnPid === "number" ? __sxnPid : 0;
  process.cwd = __sxnCwd;
  // Node has chdir, and this runtime now needs one anyway: it is what tells
  // the cached cwd it is stale.
  process.chdir = __sxnChdir;
  process.exit = function (code) { __sxnExit(code === undefined ? 0 : code); };
  // A genuine job-queue microtask (queueMicrotask is itself a thin JS_EnqueueJob
  // wrapper built into quickjs.c), not a timer -- so nextTick callbacks always
  // run before any subsequently-scheduled timer/I-O callback, matching Node's
  // "runs before the event loop continues" contract for the cases this runtime
  // supports.
  // Native (js_next_tick in src/node.c): this copied `arguments` into an
  // array and built a closure over it for every tick.
  process.nextTick = __sxnNextTick;

  // process.on('SIGINT'/'SIGTERM', ...) only arms the native libuv signal
  // watcher (see __sxnWatchSignal in src/node.c) the first time a listener
  // is added for that signal, matching Node's semantics of leaving default
  // OS signal disposition alone until the script opts in.
  var armedSignals = Object.create(null);
  var baseOn = EventEmitter.prototype.on;
  process.on = function (type, listener) {
    if ((type === "SIGINT" || type === "SIGTERM") && !armedSignals[type]) {
      armedSignals[type] = true;
      __sxnWatchSignal(type, function () { process.emit(type); });
    }
    return baseOn.call(this, type, listener);
  };
  process.addListener = process.on;

  globalThis.process = process;

  // ---------------- fs / fs/promises ----------------
  // writeFileSync/existsSync/writeFile are pure passthroughs with no logic
  // of their own, so they're bound directly to native exports (src/node.c's
  // __sxnWriteFileSync/__sxnExistsSync, src/network.c's __sxnWriteFileAsync)
  // -- phase 2 of replacing node_compat.js with native C. readFileSync/
  // readFile still branch on encoding and construct a Buffer, so they stay
  // JS until Buffer itself goes native. Encoding support is limited to
  // utf-8 (the only encoding TextDecoder/TextEncoder in bootstrap.js
  // support); without an encoding argument, reads return a Buffer, but
  // bytes still round-trip through UTF-8 decode/encode -- exact for text
  // content and lossy only for non-UTF-8 binary content.
  function wantsText(encoding) {
    return typeof encoding === "string" || (encoding && typeof encoding.encoding === "string");
  }
  // A file's size, kind and times. The numbers come from libuv; the methods
  // and the Date fields are the shape Node hands back.
  var S_IFMT = 0o170000, S_IFREG = 0o100000, S_IFDIR = 0o040000, S_IFLNK = 0o120000;
  var S_IFCHR = 0o020000, S_IFBLK = 0o060000, S_IFIFO = 0o010000, S_IFSOCK = 0o140000;
  // Native: __sxnStat fills the object itself, on this prototype, rather
  // than handing back a plain one whose every field was then copied across
  // by a for-in loop here.
  function Stats() {}
  Stats.prototype.isFile = function () { return (this.mode & S_IFMT) === S_IFREG; };
  Stats.prototype.isDirectory = function () { return (this.mode & S_IFMT) === S_IFDIR; };
  Stats.prototype.isSymbolicLink = function () { return (this.mode & S_IFMT) === S_IFLNK; };
  Stats.prototype.isCharacterDevice = function () { return (this.mode & S_IFMT) === S_IFCHR; };
  Stats.prototype.isBlockDevice = function () { return (this.mode & S_IFMT) === S_IFBLK; };
  Stats.prototype.isFIFO = function () { return (this.mode & S_IFMT) === S_IFIFO; };
  Stats.prototype.isSocket = function () { return (this.mode & S_IFMT) === S_IFSOCK; };

  var fs = {
    readFileSync: function (path, encoding) {
      var bytes = __sxnReadFileSync(path);
      if (wantsText(encoding)) return new TextDecoder().decode(bytes);
      return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    },
    writeFileSync: globalThis.__sxnWriteFileSync,
    existsSync: globalThis.__sxnExistsSync,
    statSync: function (path) { return __sxnStat(path, true, Stats.prototype); },
    lstatSync: function (path) { return __sxnStat(path, false, Stats.prototype); },
    Stats: Stats,
    // The whole file, handed to a Readable in one chunk. Enough for serving
    // a file, which is what this exists for; it is not a window onto a file
    // too large to hold.
    createReadStream: function (path, options) {
      var stream = new Readable();
      queueMicrotask(function () {
        try {
          var bytes = __sxnReadFileSync(path);
          var start = (options && options.start) || 0;
          var end = options && options.end !== undefined ? options.end + 1 : bytes.byteLength;
          var slice = bytes.subarray(start, end);
          var encoding = options && (typeof options === "string" ? options : options.encoding);
          stream.push(encoding ? new TextDecoder().decode(slice)
                               : Buffer.from(slice.buffer, slice.byteOffset, slice.byteLength));
          stream.push(null);
        } catch (e) {
          stream.emit("error", e);
        }
      });
      return stream;
    },
  };
  // The flag numbers come from this platform's headers (js_fs_constants in
  // src/network.c), which is the only way to get O_CREAT right everywhere.
  fs.constants = __sxnFsConstants();
  globalThis.__sxnFs = fs;
  delete globalThis.__sxnWriteFileSync;
  delete globalThis.__sxnExistsSync;

  var fsPromises = {
    // The same native read the synchronous side uses. This went through
    // Sxn.file().text(), which decodes the file as UTF-8 and then had to
    // encode it back to bytes -- so any byte that is not valid UTF-8 came
    // back as the replacement character, and a binary file was corrupted.
    readFile: function (path, encoding) {
      try { return Promise.resolve(fs.readFileSync(path, encoding)); }
      catch (e) { return Promise.reject(e); }
    },
    writeFile: __sxnWriteFileAsync,
    stat: function (path) {
      try { return Promise.resolve(__sxnStat(path, true, Stats.prototype)); }
      catch (e) { return Promise.reject(e); }
    },
    lstat: function (path) {
      try { return Promise.resolve(__sxnStat(path, false, Stats.prototype)); }
      catch (e) { return Promise.reject(e); }
    },
  };
  globalThis.__sxnFsPromises = fsPromises;


  // ---------------- node:stream ----------------
  // Node's EventEmitter-shaped streams, on top of the same EventEmitter this
  // runtime already ships. Flowing mode ('data' listeners and resume) and
  // paused mode (read()) both work, as do pipe, backpressure through the
  // write callback, and the Web Streams bridges.
  const EE = globalThis.__sxnEventEmitter;

  function inheritEE(Ctor) {
    Ctor.prototype = Object.create(EE.prototype);
    Ctor.prototype.constructor = Ctor;
  }

  function Readable(options) {
    EE.call(this);
    options = options || {};
    this._buf = [];
    this._bufAt = 0;      // read cursor: shift() on a long queue copies it
    this._flowing = false;
    this._ended = false;
    this._endEmitted = false;
    this._destroyed = false;
    this.readable = true;
    this._readableObjectMode = !!options.objectMode;
    this._encoding = options.encoding || null;
    if (typeof options.read === "function") this._read = options.read;
    if (typeof options.destroy === "function") this._destroyImpl = options.destroy;
  }
  inheritEE(Readable);
  Readable.prototype._read = function () {};
  Readable.prototype.push = function (chunk) {
    if (chunk !== null && badChunk(this, chunk)) throw chunkTypeError(chunk);
    if (chunk === null) {
      this._ended = true;
      maybeEndReadable(this);
      return false;
    }
    if (this._encoding && chunk instanceof Uint8Array) {
      chunk = new TextDecoder(this._encoding).decode(chunk);
    } else if (!this._readableObjectMode) {
      // Outside objectMode a stream carries bytes, so a string becomes a
      // Buffer -- consumers call chunk.toString() and expect one.
      if (typeof chunk === "string") chunk = Buffer.from(chunk, "utf-8");
      else if (chunk instanceof ArrayBuffer) chunk = Buffer.from(chunk);
      else if (chunk instanceof Uint8Array && !Buffer.isBuffer(chunk))
        chunk = Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    }
    this._buf.push(chunk);
    if (this._flowing) drainReadable(this);
    else this.emit("readable");
    return this._buf.length - this._bufAt < 16;      // a coarse high-water mark
  };
  // Chunks leave through a cursor rather than shift(), which copies the
  // whole queue down by one every time and made draining a long one cost
  // its own length squared.
  function takeChunk(r) {
    const chunk = r._buf[r._bufAt];
    r._buf[r._bufAt++] = undefined;
    if (r._bufAt === r._buf.length) { r._buf.length = 0; r._bufAt = 0; }
    return chunk;
  }
  function bufferedCount(r) { return r._buf.length - r._bufAt; }
  function drainReadable(r) {
    while (r._flowing && bufferedCount(r) > 0) r.emit("data", takeChunk(r));
    maybeEndReadable(r);
    if (r._flowing && !r._ended) r._read();
  }
  function maybeEndReadable(r) {
    if (r._ended && bufferedCount(r) === 0 && !r._endEmitted) {
      r._endEmitted = true;
      r.readable = false;
      queueMicrotask(() => { r.emit("end"); r.emit("close"); });
    }
  }
  Readable.prototype.read = function () {
    if (bufferedCount(this) === 0) { this._read(); }
    if (bufferedCount(this) === 0) { maybeEndReadable(this); return null; }
    let c;
    if (this._readableObjectMode || this._encoding || bufferedCount(this) === 1) {
      c = takeChunk(this);
    } else {
      // Node hands back everything buffered as one Buffer.
      c = Buffer.concat(this._buf.splice(this._bufAt));
      this._buf.length = 0;
      this._bufAt = 0;
    }
    maybeEndReadable(this);
    return c;
  };
  Readable.prototype.pause = function () { this._flowing = false; return this; };
  Readable.prototype.resume = function () {
    if (!this._flowing) { this._flowing = true; queueMicrotask(() => drainReadable(this)); }
    return this;
  };
  Readable.prototype.setEncoding = function (enc) { this._encoding = enc; return this; };
  Readable.prototype.destroy = function (err) {
    if (this._destroyed) return this;
    this._destroyed = true;
    this.readable = false;
    if (this._destroyImpl) this._destroyImpl(err || null, () => {});
    queueMicrotask(() => { if (err) this.emit("error", err); this.emit("close"); });
    return this;
  };
  // Native (js_stream_pipe in src/node.c): four closures per pipe, one per
  // event, became four C functions sharing the source and the destination.
  // The record kept for unpipe has the same shape it always had.
  Readable.prototype.pipe = __sxnPipe;
  // Node's readable.unpipe([dest]). finalhandler calls it before draining a
  // request it is about to answer, so it has to exist even on a stream that
  // was never piped anywhere.
  Readable.prototype.unpipe = function (dest) {
    const pipes = this._pipes;
    if (!pipes) return this;
    const keep = [];
    for (const p of pipes) {
      if (dest !== undefined && p.dest !== dest) { keep.push(p); continue; }
      this.off("data", p.onData);
      this.off("end", p.onEnd);
      this.off("error", p.onError);
      p.dest.off("drain", p.onDrain);
      p.dest.emit("unpipe", this);
    }
    this._pipes = keep;
    return this;
  };
  // `on('data')` is what switches a stream into flowing mode in Node.
  const readableOn = Readable.prototype.on;
  Readable.prototype.on = function (event, fn) {
    const r = EE.prototype.on.call(this, event, fn);
    if (event === "data") this.resume();
    return r;
  };
  void readableOn;
  Readable.prototype[Symbol.asyncIterator] = function () {
    const self = this;
    return {
      next() {
        if (bufferedCount(self) > 0) return Promise.resolve({ value: takeChunk(self), done: false });
        if (self._ended) return Promise.resolve({ value: undefined, done: true });
        return new Promise((resolve, reject) => {
          const onData = (c) => { cleanup(); resolve({ value: c, done: false }); };
          const onEnd = () => { cleanup(); resolve({ value: undefined, done: true }); };
          const onErr = (e) => { cleanup(); reject(e); };
          const cleanup = () => {
            self.off("data", onData); self.off("end", onEnd); self.off("error", onErr);
          };
          self.on("data", onData); self.on("end", onEnd); self.on("error", onErr);
        });
      },
      [Symbol.asyncIterator]() { return this; },
    };
  };
  Readable.from = function (iterable, options) {
    const it = iterable[Symbol.asyncIterator]
      ? iterable[Symbol.asyncIterator]() : iterable[Symbol.iterator]();
    const r = new Readable(Object.assign({ objectMode: true }, options));
    let pulling = false;
    r._read = function () {
      if (pulling) return;
      pulling = true;
      Promise.resolve(it.next()).then(({ value, done }) => {
        pulling = false;
        if (done) r.push(null); else r.push(value);
      }, (e) => { pulling = false; r.destroy(e); });
    };
    return r;
  };
  Readable.fromWeb = function (webStream, options) {
    const reader = webStream.getReader();
    const r = new Readable(options);
    let pulling = false;
    r._read = function () {
      if (pulling) return;
      pulling = true;
      reader.read().then(({ value, done }) => {
        pulling = false;
        if (done) r.push(null); else r.push(value);
      }, (e) => { pulling = false; r.destroy(e); });
    };
    return r;
  };
  Readable.toWeb = function (nodeStream) {
    return new ReadableStream({
      start(c) {
        nodeStream.on("data", (chunk) => c.enqueue(chunk));
        nodeStream.on("end", () => { try { c.close(); } catch {} });
        nodeStream.on("error", (e) => c.error(e));
      },
      cancel() { nodeStream.destroy(); },
    });
  };

  function Writable(options) {
    EE.call(this);
    options = options || {};
    this._writableObjectMode = !!options.objectMode;
    this.writable = true;
    this._destroyed = false;
    this._finished = false;
    this._corked = 0;
    if (typeof options.write === "function") this._write = options.write;
    if (typeof options.final === "function") this._final = options.final;
    if (typeof options.destroy === "function") this._destroyImpl = options.destroy;
  }
  inheritEE(Writable);
  Writable.prototype._write = function (chunk, enc, cb) { cb(); };
  function chunkTypeError(chunk) {
    const e = new TypeError(
      'The "chunk" argument must be of type string or an instance of Buffer, ' +
      "TypedArray, or DataView. Received type " + typeof chunk + " (" + String(chunk) + ")");
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  }
  function badChunk(stream, chunk) {
    // Outside objectMode a stream carries bytes, and Node refuses anything
    // else rather than stringifying it silently.
    if (stream._writableObjectMode || stream._readableObjectMode) return false;
    return !(typeof chunk === "string" || chunk instanceof Uint8Array ||
             chunk instanceof ArrayBuffer || ArrayBuffer.isView(chunk));
  }
  Writable.prototype.write = function (chunk, enc, cb) {
    if (typeof enc === "function") { cb = enc; enc = undefined; }
    if (badChunk(this, chunk)) throw chunkTypeError(chunk);
    if (!this.writable) {
      const e = new Error("write after end");
      queueMicrotask(() => this.emit("error", e));
      if (cb) cb(e);
      return false;
    }
    // Native (sxn_write_done in src/node.c): the callback _write is handed
    // was a JavaScript closure per chunk. An error still reaches the
    // stream's 'error' listeners whether or not a callback was passed.
    this._write(chunk, enc || "utf8", __sxnWriteCallback(this, cb));
    return true;
  };
  Writable.prototype.cork = function () { this._corked++; };
  Writable.prototype.uncork = function () { if (this._corked) this._corked--; };
  Writable.prototype.end = function (chunk, enc, cb) {
    if (typeof chunk === "function") { cb = chunk; chunk = undefined; }
    if (typeof enc === "function") { cb = enc; enc = undefined; }
    if (chunk !== undefined && chunk !== null) this.write(chunk, enc);
    if (!this.writable) { if (cb) cb(); return this; }
    this.writable = false;
    const done = () => {
      this._finished = true;
      if (cb) cb();
      this.emit("finish");
      this.emit("close");
    };
    if (this._final) this._final((err) => { if (err) this.emit("error", err); else done(); });
    else queueMicrotask(done);
    return this;
  };
  Writable.prototype.destroy = function (err) {
    if (this._destroyed) return this;
    this._destroyed = true;
    this.writable = false;
    if (this._destroyImpl) this._destroyImpl(err || null, () => {});
    queueMicrotask(() => { if (err) this.emit("error", err); this.emit("close"); });
    return this;
  };
  Writable.toWeb = function (nodeWritable) {
    return new WritableStream({
      write(chunk) { return new Promise((res, rej) => nodeWritable.write(chunk, (e) => e ? rej(e) : res())); },
      close() { return new Promise((res) => nodeWritable.end(res)); },
      abort(reason) { nodeWritable.destroy(reason); },
    });
  };

  // Duplex is both halves on one object; Transform is a Duplex whose write
  // feeds its own readable side through _transform.
  function Duplex(options) {
    Readable.call(this, options);
    Writable.call(this, options);
    if (options && typeof options.write === "function") this._write = options.write;
    if (options && typeof options.read === "function") this._read = options.read;
  }
  Duplex.prototype = Object.create(Readable.prototype);
  Duplex.prototype.constructor = Duplex;
  for (const k of ["write", "end", "cork", "uncork", "_write"])
    Duplex.prototype[k] = Writable.prototype[k];
  Object.defineProperty(Duplex.prototype, "writable", {
    get() { return this._writable !== false; },
    set(v) { this._writable = v; },
    configurable: true,
  });

  function Transform(options) {
    Duplex.call(this, options);
    this._writableObjectMode = !!(options && options.objectMode);
    this._readableObjectMode = !!(options && options.objectMode);
    if (options && typeof options.transform === "function") this._transform = options.transform;
    if (options && typeof options.flush === "function") this._flush = options.flush;
  }
  Transform.prototype = Object.create(Duplex.prototype);
  Transform.prototype.constructor = Transform;
  Transform.prototype._transform = function (chunk, enc, cb) { cb(null, chunk); };
  Transform.prototype._write = function (chunk, enc, cb) {
    this._transform(chunk, enc, (err, out) => {
      if (err) return cb(err);
      if (out !== undefined && out !== null) this.push(out);
      cb();
    });
  };
  Transform.prototype.end = function (chunk, enc, cb) {
    if (typeof chunk === "function") { cb = chunk; chunk = undefined; }
    if (chunk !== undefined && chunk !== null) this.write(chunk, enc);
    const finish = () => { this.push(null); if (cb) cb(); this.emit("finish"); };
    if (this._flush) this._flush((err, out) => {
      if (err) return this.emit("error", err);
      if (out !== undefined && out !== null) this.push(out);
      finish();
    });
    else queueMicrotask(finish);
    return this;
  };

  function PassThrough(options) { Transform.call(this, options); }
  PassThrough.prototype = Object.create(Transform.prototype);
  PassThrough.prototype.constructor = PassThrough;

  function finished(stream, cb) {
    const done = (err) => { cleanup(); cb ? cb(err) : undefined; if (!cb) throw err; };
    const onEnd = () => { cleanup(); if (cb) cb(null); };
    const onErr = (e) => { cleanup(); if (cb) cb(e); };
    const cleanup = () => {
      stream.off && stream.off("end", onEnd);
      stream.off && stream.off("finish", onEnd);
      stream.off && stream.off("error", onErr);
    };
    void done;
    stream.on("end", onEnd); stream.on("finish", onEnd); stream.on("error", onErr);
  }

  function pipeline(...args) {
    let cb = typeof args[args.length - 1] === "function" ? args.pop() : null;
    const streams = args;
    let cur = streams[0];
    for (let i = 1; i < streams.length; i++) cur = cur.pipe(streams[i]);
    const last = streams[streams.length - 1];
    if (cb) finished(last, cb);
    return last;
  }

  // Node's node:stream *is* the Stream constructor, with the classes hung off
  // it -- `util.inherits(MyStream, require('stream'))` is a common idiom and
  // needs a function with a prototype, not a plain namespace object.
  function Stream(options) { EE.call(this); void options; }
  inheritEE(Stream);
  Stream.prototype.pipe = Readable.prototype.pipe;

  const streamModule = Object.assign(Stream, {
    Readable, Writable, Duplex, Transform, PassThrough, Stream,
    pipeline, finished,
    promises: {
      pipeline: (...a) => new Promise((res, rej) =>
        pipeline(...a, (e) => (e ? rej(e) : res(undefined)))),
      finished: (s) => new Promise((res, rej) => finished(s, (e) => (e ? rej(e) : res(undefined)))),
    },
  });
  globalThis.__sxnStream = streamModule;
  globalThis.__sxnStreamPromises = streamModule.promises;


  // ---------------- node:http ----------------
  // Node's (req, res) server on top of Sxn.serve. The handler here returns a
  // promise that settles when res.end() runs, which is what lets an
  // application write its response whenever it likes rather than returning
  // one -- the shape Express and everything like it is built around.
  const STATUS_CODES = {
    200: "OK", 201: "Created", 202: "Accepted", 204: "No Content",
    301: "Moved Permanently", 302: "Found", 303: "See Other", 304: "Not Modified",
    307: "Temporary Redirect", 308: "Permanent Redirect",
    400: "Bad Request", 401: "Unauthorized", 403: "Forbidden", 404: "Not Found",
    405: "Method Not Allowed", 409: "Conflict", 410: "Gone",
    413: "Payload Too Large", 415: "Unsupported Media Type", 418: "I'm a Teapot",
    422: "Unprocessable Entity", 429: "Too Many Requests",
    500: "Internal Server Error", 501: "Not Implemented", 502: "Bad Gateway",
    503: "Service Unavailable", 504: "Gateway Timeout",
  };

  function IncomingMessage(raw) {
    Readable.call(this, {});
    this.method = raw.method || "GET";
    this.url = raw.url || "/";
    // Native (js_http_headers in src/node.c): lowercase every name and
    // flatten into rawHeaders in one pass, once per request.
    const named = __sxnHttpHeaders(raw.headers);
    this.headers = named.headers;
    this.httpVersion = "1.1";
    this.rawHeaders = named.rawHeaders;
    // on-finished reads socket.readable and `complete` to decide whether a
    // request is spent; body-parser skips parsing when it says yes, so both
    // have to describe a request whose body has not been read yet. The socket
    // is a real emitter because on-finished subscribes to its 'error' and
    // 'close', and a plain object had no on() for it to call.
    // Native (js_http_socket in src/node.c): the shape is the same every
    // request, so the prototype is built once rather than eleven properties
    // being copied onto a fresh emitter each time.
    this.socket = __sxnHttpSocket();
    this.connection = this.socket;
    this.complete = false;
    // Native, and shared: this was an arrow function per request.
    this.once("end", __sxnHttpComplete);
    // The body is already off the wire, but it must not be pushed before the
    // consumer attaches: body-parser adds its 'data' listener after the
    // handler returns, and an eagerly-ended stream would hand it nothing.
    // Pushing from _read defers until something actually reads.
    this._rawBody = raw.body;
    this._bodySent = false;
  }
  IncomingMessage.prototype = Object.create(Readable.prototype);
  IncomingMessage.prototype.constructor = IncomingMessage;
  // Native (js_http_read_body in src/node.c), and on the prototype rather
  // than a closure built per request.
  IncomingMessage.prototype._read = __sxnHttpReadBody;

  function ServerResponse(settle) {
    Writable.call(this, {});
    this.statusCode = 200;
    this.statusMessage = undefined;
    this.headersSent = false;
    this.finished = false;
    this._headers = {};
    this._chunks = [];
    this._settle = settle;
  }
  ServerResponse.prototype = Object.create(Writable.prototype);
  ServerResponse.prototype.constructor = ServerResponse;
  // Native (js_header_op in src/node.c): each of these lowercases the name
  // first, which is a scan over a short string rather than a builtin call.
  ServerResponse.prototype.setHeader = __sxnSetHeader;
  ServerResponse.prototype.getHeader = __sxnGetHeader;
  ServerResponse.prototype.getHeaders = __sxnGetHeaders;
  ServerResponse.prototype.getHeaderNames = __sxnGetHeaderNames;
  ServerResponse.prototype.hasHeader = __sxnHasHeader;
  ServerResponse.prototype.removeHeader = __sxnRemoveHeader;
  ServerResponse.prototype.writeHead = function (status, reasonOrHeaders, maybeHeaders) {
    this.statusCode = status;
    let headers = maybeHeaders;
    if (typeof reasonOrHeaders === "string") this.statusMessage = reasonOrHeaders;
    else headers = reasonOrHeaders || maybeHeaders;
    if (headers) for (const k of Object.keys(headers)) this.setHeader(k, headers[k]);
    this.headersSent = true;
    return this;
  };
  ServerResponse.prototype.write = function (chunk, enc, cb) {
    if (typeof enc === "function") { cb = enc; enc = undefined; }
    if (chunk !== undefined && chunk !== null) this._chunks.push(chunk);
    this.headersSent = true;
    if (cb) queueMicrotask(cb);
    return true;
  };
  ServerResponse.prototype.end = function (chunk, enc, cb) {
    if (typeof chunk === "function") { cb = chunk; chunk = undefined; }
    if (typeof enc === "function") { cb = enc; enc = undefined; }
    if (this.finished) return this;
    if (chunk !== undefined && chunk !== null) this._chunks.push(chunk);
    this.finished = true;
    this.headersSent = true;
    // Concatenate once. Native (js_join_chunks in src/node.c) answers the
    // cases every real response is -- nothing written, one string, all bytes
    // -- and hands back undefined for a mix, where joining strings is the
    // engine's own job.
    let body = __sxnJoinChunks(this._chunks);
    if (body === undefined) {
      const anyBinary = this._chunks.some((c) => c instanceof Uint8Array || c instanceof ArrayBuffer);
      if (!anyBinary) body = this._chunks.map((c) => String(c)).join("");
      else {
      body = __sxnConcatBytes(this._chunks.map((c) =>
        c instanceof Uint8Array ? c
        : c instanceof ArrayBuffer ? new Uint8Array(c)
        : new TextEncoder().encode(String(c))));
      }
    }
    this._settle({ statusCode: this.statusCode, headers: this._headers, body });
    if (cb) queueMicrotask(cb);
    this.emit("finish");
    this.emit("close");
    return this;
  };

  function Server(handler) {
    EE.call(this);
    this._handler = handler || null;
    this._native = null;
    if (handler) this.on("request", handler);
  }
  inheritEE(Server);
  Server.prototype.listen = function (port, hostOrCb, cb) {
    if (typeof hostOrCb === "function") { cb = hostOrCb; }
    if (typeof port === "object" && port !== null) port = port.port;
    const self = this;
    // __sxnServe, not Sxn.serve: IncomingMessage wants the native layer's
    // plain {method, url, headers, body} object, and a Node req.url is a
    // path. Sxn.serve now hands its handler a Fetch Request instead.
    this._native = __sxnServe({ port: Number(port) || 0 }, (raw) =>
      new Promise((resolve) => {
        const req = new IncomingMessage(raw);
        const res = new ServerResponse(resolve);
        self.emit("request", req, res);
      }));
    this._port = this._native.port;
    if (cb) queueMicrotask(() => cb());
    queueMicrotask(() => self.emit("listening"));
    return this;
  };
  Server.prototype.address = function () {
    return this._native ? { address: "127.0.0.1", family: "IPv4", port: this._native.port } : null;
  };
  Server.prototype.close = function (cb) {
    if (this._native) { this._native.stop(); this._native = null; }
    if (cb) queueMicrotask(() => cb());
    queueMicrotask(() => this.emit("close"));
    return this;
  };

  // The client half maps onto fetch, which is the transport this runtime has.
  function ClientRequest(options, cb) {
    Writable.call(this, {});
    const opts = typeof options === "string" ? { url: options } : options || {};
    this._url = opts.url ||
      ((opts.protocol || "http:") + "//" + (opts.hostname || opts.host || "localhost") +
       (opts.port ? ":" + opts.port : "") + (opts.path || "/"));
    this._method = opts.method || "GET";
    this._headers = Object.assign({}, opts.headers);
    this._body = [];
    if (cb) this.once("response", cb);
  }
  ClientRequest.prototype = Object.create(Writable.prototype);
  ClientRequest.prototype.constructor = ClientRequest;
  ClientRequest.prototype.setHeader = function (k, v) { this._headers[k] = v; return this; };
  ClientRequest.prototype.write = function (chunk) { this._body.push(chunk); return true; };
  ClientRequest.prototype.end = function (chunk) {
    if (chunk !== undefined && chunk !== null) this._body.push(chunk);
    const body = this._body.length ? this._body.map(String).join("") : undefined;
    fetch(this._url, { method: this._method, headers: this._headers, body }).then(async (r) => {
      const res = new IncomingMessage({
        method: this._method, url: this._url,
        headers: Object.fromEntries(r.headers), body: await r.text(),
      });
      res.statusCode = r.status;
      res.statusMessage = r.statusText;
      this.emit("response", res);
    }, (e) => this.emit("error", e));
    return this;
  };

  const http = {
    STATUS_CODES,
    METHODS: ["DELETE","GET","HEAD","OPTIONS","PATCH","POST","PUT"],
    IncomingMessage, ServerResponse, Server,
    createServer(opts, handler) {
      if (typeof opts === "function") { handler = opts; }
      return new Server(handler);
    },
    request(options, cb) { return new ClientRequest(options, cb); },
    get(options, cb) { const r = new ClientRequest(options, cb); r.end(); return r; },
  };
  globalThis.__sxnHttp = http;





  // ---------------- node:net ----------------
  // Address validation is what frameworks import this for -- Express uses
  // net.isIP when parsing X-Forwarded-For. Real sockets are not implemented,
  // and say so rather than pretending to connect.
  // Native (js_net_is_ip in src/node.c): the system's own address parser,
  // rather than two regexps and a split per call.
  const isIPv4 = (s) => __sxnIsIP(s) === 4;
  const isIPv6 = (s) => __sxnIsIP(s) === 6;

  const net = {
    isIP: __sxnIsIP,
    isIPv4, isIPv6,
    Socket: function Socket() { throw new Error("net.Socket is not implemented"); },
    Server: function Server() { throw new Error("net.Server is not implemented; use node:http"); },
    createConnection() { throw new Error("net.createConnection is not implemented"); },
    connect() { throw new Error("net.connect is not implemented"); },
    createServer() { throw new Error("net.createServer is not implemented; use node:http"); },
  };
  globalThis.__sxnNet = net;

  // ---------------- node:crypto ----------------
  // The synchronous surface packages reach for: hashes, HMAC, random bytes
  // and a constant-time compare. Digests come from the same OpenSSL binding
  // WebCrypto uses; HMAC is the standard construction over it, which needs no
  // second binding and is exactly what the RFC specifies.
  const cryptoToBytes = (d, enc) => {
    if (typeof d === "string") {
      // Buffer's readers are the native ones; this used to parse hex a byte
      // at a time with parseInt and read base64 through atob.
      if (enc && enc !== "utf8" && enc !== "utf-8") return bufferBytesFromString(d, enc);
      return new TextEncoder().encode(d);
    }
    if (d instanceof ArrayBuffer) return new Uint8Array(d);
    if (ArrayBuffer.isView(d)) return new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
    throw new TypeError("expected a string, Buffer or TypedArray");
  };
  // Uint8Array's own toHex/toBase64 are native; building the hex by hand cost
  // a string per byte, an array and a join -- which was most of the time a
  // digest took once the digest itself was C.
  const encodeDigest = (bytes, encoding) => {
    if (!encoding || encoding === "buffer")
      return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (encoding === "hex") return bytes.toHex();
    if (encoding === "base64") return bytes.toBase64();
    if (encoding === "base64url") return bytes.toBase64({ alphabet: "base64url", omitPadding: true });
    throw new TypeError("unsupported digest encoding: " + encoding);
  };
  const concatBytes = __sxnConcatBytes;

  function Hash(algorithm) {
    this._algo = String(algorithm).toLowerCase();
    this._parts = [];
  }
  Hash.prototype.update = function (data, enc) {
    this._parts.push(cryptoToBytes(data, enc));
    return this;
  };
  Hash.prototype.digest = function (encoding) {
    const data = this._parts.length === 1 ? this._parts[0] : concatBytes(this._parts);
    return encodeDigest(__sxnDigest(this._algo, data), encoding);
  };
  Hash.prototype.copy = function () {
    const h = new Hash(this._algo);
    h._parts = this._parts.slice();
    return h;
  };

  // Native (sxn_hmac in src/network.c): OpenSSL's own HMAC, rather than two
  // padded key buffers and three digest calls built here.
  function Hmac(algorithm, key) {
    this._algo = String(algorithm).toLowerCase();
    this._key = cryptoToBytes(key);
    this._parts = [];
  }
  Hmac.prototype.update = function (data, enc) {
    this._parts.push(cryptoToBytes(data, enc));
    return this;
  };
  Hmac.prototype.digest = function (encoding) {
    // One update is the usual case, and then there is nothing to join.
    const data = this._parts.length === 1 ? this._parts[0] : concatBytes(this._parts);
    return encodeDigest(__sxnHmac(this._algo, this._key, data), encoding);
  };

  const nodeCrypto = {
    createHash: (algorithm) => new Hash(algorithm),
    createHmac: (algorithm, key) => new Hmac(algorithm, key),
    randomBytes(size, cb) {
      const b = Buffer.from(__sxnRandomBytes(size));
      if (cb) { queueMicrotask(() => cb(null, b)); return undefined; }
      return b;
    },
    randomUUID: () => globalThis.crypto.randomUUID(),
    randomFillSync(buf) { globalThis.crypto.getRandomValues(buf); return buf; },
    getRandomValues: (a) => globalThis.crypto.getRandomValues(a),
    randomInt(min, max) {
      if (max === undefined) { max = min; min = 0; }
      const range = max - min;
      const b = __sxnRandomBytes(6);
      let v = 0; for (const x of b) v = v * 256 + x;
      return min + (v % range);
    },
    // Compares in time independent of where the first difference is.
    // Native: a comparison that takes the same time either way is not
    // something a JavaScript loop can promise.
    timingSafeEqual: (a, b) => __sxnTimingSafeEqual(cryptoToBytes(a), cryptoToBytes(b)),
    getHashes: () => ["md5", "sha1", "sha224", "sha256", "sha384", "sha512"],
    constants: {},
    webcrypto: globalThis.crypto,
    subtle: globalThis.crypto && globalThis.crypto.subtle,
    Hash, Hmac,
  };
  globalThis.__sxnCrypto = nodeCrypto;

  // ---------------- node:zlib ----------------
  // gzip, deflate and their raw form, sync and callback and promise, plus the
  // Transform streams. The three pairs differ only in the container zlib puts
  // around the same deflate data, which windowBits selects: 15 zlib, 31 gzip,
  // -15 raw.
  const Z = { ZLIB: 15, GZIP: 31, RAW: -15 };
  const toBytes = (input) => {
    if (typeof input === "string") return new TextEncoder().encode(input);
    if (input instanceof ArrayBuffer) return new Uint8Array(input);
    if (ArrayBuffer.isView(input))
      return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    throw new TypeError("zlib input must be a string, Buffer, TypedArray or ArrayBuffer");
  };
  const asBuffer = (u8) => Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength);

  function makeSync(bits, compress) {
    return function (input, options) {
      const level = options && options.level !== undefined ? options.level : undefined;
      const out = compress
        ? __sxnZlibDeflate(toBytes(input), bits, level)
        : __sxnZlibInflate(toBytes(input), bits);
      return asBuffer(out);
    };
  }
  // The callback form is Node's (err, result); errors arrive as a rejection
  // rather than a throw, so a bad payload does not take the process down.
  function makeAsync(syncFn) {
    return function (input, options, cb) {
      if (typeof options === "function") { cb = options; options = undefined; }
      queueMicrotask(() => {
        try { cb(null, syncFn(input, options)); }
        catch (e) { cb(e); }
      });
    };
  }

  const zlib = {
    constants: {
      Z_NO_COMPRESSION: 0, Z_BEST_SPEED: 1, Z_BEST_COMPRESSION: 9,
      Z_DEFAULT_COMPRESSION: -1, Z_NO_FLUSH: 0, Z_FINISH: 4,
      Z_OK: 0, Z_STREAM_END: 1,
    },
    gzipSync: makeSync(Z.GZIP, true),
    gunzipSync: makeSync(Z.GZIP, false),
    deflateSync: makeSync(Z.ZLIB, true),
    inflateSync: makeSync(Z.ZLIB, false),
    deflateRawSync: makeSync(Z.RAW, true),
    inflateRawSync: makeSync(Z.RAW, false),
    unzipSync: function (input, options) {
      // unzip accepts either container, so try gzip and fall back to zlib.
      try { return zlib.gunzipSync(input, options); }
      catch { return zlib.inflateSync(input, options); }
    },
  };
  zlib.gzip = makeAsync(zlib.gzipSync);
  zlib.gunzip = makeAsync(zlib.gunzipSync);
  zlib.deflate = makeAsync(zlib.deflateSync);
  zlib.inflate = makeAsync(zlib.inflateSync);
  zlib.deflateRaw = makeAsync(zlib.deflateRawSync);
  zlib.inflateRaw = makeAsync(zlib.inflateRawSync);
  zlib.unzip = makeAsync(zlib.unzipSync);

  // Streaming: buffer the chunks and convert on flush. zlib's own streaming
  // state is not exposed here, and a whole-payload conversion is equivalent
  // for anything that ends its stream, which is every consumer of these.
  function makeZlibTransform(syncFn) {
    return function (options) {
      const parts = [];
      const t = new Transform(Object.assign({}, options, {
        transform(chunk, enc, cb) { parts.push(toBytes(chunk)); cb(); },
        flush(cb) {
          try {
            cb(null, syncFn(__sxnConcatBytes(parts), options));
          } catch (e) { cb(e); }
        },
      }));
      return t;
    };
  }
  zlib.createGzip = makeZlibTransform(zlib.gzipSync);
  zlib.createGunzip = makeZlibTransform(zlib.gunzipSync);
  zlib.createDeflate = makeZlibTransform(zlib.deflateSync);
  zlib.createInflate = makeZlibTransform(zlib.inflateSync);
  zlib.createDeflateRaw = makeZlibTransform(zlib.deflateRawSync);
  zlib.createInflateRaw = makeZlibTransform(zlib.inflateRawSync);
  zlib.createUnzip = makeZlibTransform(zlib.unzipSync);
  // Node has no zlib.promises namespace -- the callback forms are promisified
  // with util.promisify, so adding one here would be an invention.
  globalThis.__sxnZlib = zlib;

  // ---------------- small node builtins ----------------
  // Enough of each for the packages that reach for them in passing: `debug`
  // wants tty.isatty, body-parser wants string_decoder, and so on.
  const tty = {
    isatty: () => false,
    ReadStream: function ReadStream() { throw new Error("tty.ReadStream is not supported"); },
    WriteStream: function WriteStream() { throw new Error("tty.WriteStream is not supported"); },
  };
  globalThis.__sxnTty = tty;

  // Decodes byte chunks to text without splitting a multi-byte character
  // across a chunk boundary -- which is the entire reason it exists.
  function StringDecoder(encoding) {
    this.encoding = (encoding || "utf8").toLowerCase();
    this._tail = null;
    // utf-8 is native (js_decode_chunk in src/node.c); the other encodings
    // keep the TextDecoder, which is where they came from.
    this._dec = (this.encoding === "utf8" || this.encoding === "utf-8")
      ? null : new TextDecoder(this.encoding);
  }
  StringDecoder.prototype.write = function (buf) {
    if (typeof buf === "string") return buf;
    if (this._dec) return this._dec.decode(buf, { stream: true });
    return __sxnDecodeChunk(this, buf);
  };
  StringDecoder.prototype.end = function (buf) {
    let out = buf ? this.write(buf) : "";
    if (this._dec) return out + this._dec.decode();
    // Anything still held back was never going to complete. It is the start
    // of one character, however many bytes of it arrived, so Node emits one
    // replacement character for it.
    if (this._tail) { out += "\ufffd"; this._tail = null; }
    return out;
  };
  globalThis.__sxnStringDecoder = { StringDecoder };

  const timers = {
    setTimeout: (...a) => globalThis.setTimeout(...a),
    clearTimeout: (...a) => globalThis.clearTimeout(...a),
    setInterval: (...a) => globalThis.setInterval(...a),
    clearInterval: (...a) => globalThis.clearInterval(...a),
    setImmediate: (fn, ...a) => globalThis.setTimeout(() => fn(...a), 0),
    clearImmediate: (id) => globalThis.clearTimeout(id),
    promises: {
      setTimeout: (ms, value) => new Promise((r) => globalThis.setTimeout(() => r(value), ms)),
      setImmediate: (value) => new Promise((r) => globalThis.setTimeout(() => r(value), 0)),
    },
  };
  globalThis.__sxnTimers = timers;
  globalThis.__sxnTimersPromises = timers.promises;
  if (typeof globalThis.setImmediate === "undefined") globalThis.setImmediate = timers.setImmediate;
  if (typeof globalThis.clearImmediate === "undefined") globalThis.clearImmediate = timers.clearImmediate;

  const perfHooks = {
    performance: globalThis.performance,
    PerformanceObserver: function PerformanceObserver() {
      throw new Error("PerformanceObserver is not supported");
    },
  };
  globalThis.__sxnPerfHooks = perfHooks;

  // node:module -- createRequire is what ESM code uses to reach CommonJS.
  // Packages read `require('module').prototype.require` -- Next does it on its
  // first line -- so node:module has to be the Module constructor with the
  // rest hung off it, the same shape as node:events.
  //
  // Replacing Module.prototype.require does NOT redirect this runtime's
  // require(): the loader is native and does not consult it. A package that
  // aliases dependencies that way will find its aliases quietly ignored.
  function Module(id, parent) {
    this.id = id; this.filename = id; this.exports = {};
    this.parent = parent || null; this.loaded = false;
    this.children = []; this.paths = [];
  }
  Module.prototype.require = function (spec) {
    return (this.filename ? __sxnMakeRequire(this.filename) : globalThis.require)(spec);
  };
  Module._cache = Object.create(null);
  Module._resolveFilename = (spec) => globalThis.require.resolve(spec);
  const moduleModule = Object.assign(Module, {
    Module,
    createRequire: (from) => __sxnMakeRequire(String(from)),
    // Native, and read off the table require() itself uses (js_builtin_names
    // in src/node.c): the list here used to be written by hand and had fallen
    // short of several modules that do resolve.
    builtinModules: __sxnBuiltinNames(),
    isBuiltin: __sxnIsBuiltin,
  });
  globalThis.__sxnModule = moduleModule;

  // CommonJS reaches the builtins through require(), which is synchronous, so
  // it cannot go through import(). The mapping from specifier to builtin is
  // native (sxn_builtin_lookup in src/node.c), where the table is static
  // rather than an object rebuilt on every require() call.

  // ---------------- node:util ----------------
  // The parts packages actually import: promisify, callbackify, inherits,
  // format, deprecate, and the types guards. inspect is a readable
  // approximation, not Node's exact formatter.
  // Native (js_inspect in src/node.c). This was a recursive walk that built
  // an options object per level, a mapped array per container and a joined
  // string per level; the C one walks into a single buffer. It also answers
  // "Invalid Date" where this used to throw out of toISOString.
  const inspect = __sxnInspect;

  // Native (js_util_format in src/node.c): the scan and the substitution are
  // string work. Only the cases that need inspect -- %s of something that is
  // not a string, and %o/%O -- come back into JavaScript, which is why
  // inspect is handed over as the first argument.
  const format = (...args) => __sxnFormat(inspect, ...args);

  const util = {
    inspect,
    format,
    // Node keeps the custom-inspect symbol here.
    // Native (js_promisify in src/node.c): this spread the arguments, built
    // a Promise with an executor closure and a callback closure inside it,
    // per call. It also resolved with an array when a callback reported
    // more than one value; Node keeps the first and drops the rest.
    promisify: __sxnPromisify,
    // Native (js_callbackify in src/node.c): this built two closures per
    // call for the two halves of the promise, 1.32 microseconds to 0.84.
    callbackify: __sxnCallbackify,
    // Native (js_inherits in src/node.c): two property operations, 0.250
    // microseconds to 0.060.
    inherits: __sxnInherits,
    deprecate(fn, msg) {
      let warned = false;
      return function (...args) {
        if (!warned) { warned = true; console.error("DeprecationWarning: " + msg); }
        return fn.apply(this, args);
      };
    },
    isDeepStrictEqual(a, b) { return deepEqual(a, b, true); },
    types: {
      isDate: (v) => v instanceof Date,
      isRegExp: (v) => v instanceof RegExp,
      isMap: (v) => v instanceof Map,
      isSet: (v) => v instanceof Set,
      isPromise: (v) => v instanceof Promise,
      isTypedArray: (v) => ArrayBuffer.isView(v) && !(v instanceof DataView),
      isArrayBuffer: (v) => v instanceof ArrayBuffer,
      isDataView: (v) => v instanceof DataView,
      isNativeError: (v) => v instanceof Error,
      isAsyncFunction: (v) => typeof v === "function" && v.constructor && v.constructor.name === "AsyncFunction",
      isGeneratorFunction: (v) => typeof v === "function" && v.constructor && v.constructor.name === "GeneratorFunction",
    },
    TextEncoder: globalThis.TextEncoder,
    TextDecoder: globalThis.TextDecoder,
  };
  globalThis.__sxnUtil = util;

  // Structural equality, shared by util.isDeepStrictEqual and node:assert.
  // Native (sxn_deep_equal in src/node.c).
  const deepEqual = (a, b, strict) => strict ? __sxnDeepEqual(a, b) : __sxnLooseDeepEqual(a, b);

  // ---------------- node:assert ----------------
  function AssertionError(opts) {
    const e = new Error(opts.message ||
      (inspect(opts.actual) + " " + opts.operator + " " + inspect(opts.expected)));
    e.name = "AssertionError";
    e.code = "ERR_ASSERTION";
    e.actual = opts.actual; e.expected = opts.expected; e.operator = opts.operator;
    return e;
  }
  function assert(value, message) {
    if (!value) throw AssertionError({ message, actual: value, expected: true, operator: "==" });
  }
  assert.ok = assert;
  assert.AssertionError = AssertionError;
  assert.equal = (a, b, m) => { if (!(a == b)) throw AssertionError({ message: m, actual: a, expected: b, operator: "==" }); };
  assert.notEqual = (a, b, m) => { if (a == b) throw AssertionError({ message: m, actual: a, expected: b, operator: "!=" }); };
  assert.strictEqual = (a, b, m) => { if (!Object.is(a, b)) throw AssertionError({ message: m, actual: a, expected: b, operator: "strictEqual" }); };
  assert.notStrictEqual = (a, b, m) => { if (Object.is(a, b)) throw AssertionError({ message: m, actual: a, expected: b, operator: "notStrictEqual" }); };
  assert.deepEqual = (a, b, m) => { if (!deepEqual(a, b, false)) throw AssertionError({ message: m, actual: a, expected: b, operator: "deepEqual" }); };
  assert.deepStrictEqual = (a, b, m) => { if (!deepEqual(a, b, true)) throw AssertionError({ message: m, actual: a, expected: b, operator: "deepStrictEqual" }); };
  assert.notDeepStrictEqual = (a, b, m) => { if (deepEqual(a, b, true)) throw AssertionError({ message: m, actual: a, expected: b, operator: "notDeepStrictEqual" }); };
  assert.fail = (m) => { throw AssertionError({ message: m || "Failed", operator: "fail" }); };
  assert.throws = (fn, _expected, m) => {
    try { fn(); } catch { return; }
    throw AssertionError({ message: m || "Missing expected exception.", operator: "throws" });
  };
  assert.doesNotThrow = (fn, m) => { fn(); void m; };
  assert.match = (str, re, m) => { if (!re.test(str)) throw AssertionError({ message: m, actual: str, expected: re, operator: "match" }); };
  assert.strict = assert;
  globalThis.__sxnAssert = assert;

  // ---------------- node:os ----------------
  // Answered by libuv. These used to be guesses -- "localhost" for the
  // hostname, 0 for the memory sizes, an empty list of CPUs -- which is worse
  // than not answering at all, because nothing can tell a stub from the
  // truth.
  const uname = () => __sxnOsUname();
  const os = {
    EOL: "\n",
    platform: () => process.platform,
    arch: () => process.arch,
    type: () => uname().sysname || (process.platform === "darwin" ? "Darwin" : process.platform === "win32" ? "Windows_NT" : "Linux"),
    release: () => uname().release || "",
    version: () => uname().version || "",
    machine: () => uname().machine || process.arch,
    hostname: () => __sxnOsHostname(),
    tmpdir: () => __sxnOsTmpdir(),
    homedir: () => __sxnOsHomedir(),
    endianness: () => "LE",
    cpus: () => __sxnOsCpus(),
    availableParallelism: () => __sxnOsNumbers().parallelism,
    networkInterfaces: () => __sxnOsInterfaces(),
    totalmem: () => __sxnOsNumbers().totalmem,
    freemem: () => __sxnOsNumbers().freemem,
    loadavg: () => __sxnOsNumbers().loadavg,
    uptime: () => Math.floor(__sxnOsNumbers().uptime),
    userInfo: () => ({
      username: (process.env && (process.env.USER || process.env.USERNAME)) || "",
      homedir: __sxnOsHomedir(),
      shell: (process.env && process.env.SHELL) || null,
      uid: -1,
      gid: -1,
    }),
    devNull: "/dev/null",
    constants: { signals: {}, errno: {}, priority: {} },
  };
  globalThis.__sxnOs = os;

  // ---------------- node:querystring ----------------
  // Native (js_qs_* in src/node.c): pure string work with no state, which
  // went through split(), a regexp for "+" and decodeURIComponent per part.
  const querystring = {
    parse: __sxnQsParse,
    stringify: __sxnQsStringify,
    escape: __sxnQsEscape,
    unescape: __sxnQsUnescape,
    decode: __sxnQsParse,
    encode: __sxnQsStringify,
  };
  globalThis.__sxnQuerystring = querystring;
  delete globalThis.__sxnQsParse;
  delete globalThis.__sxnQsStringify;
  delete globalThis.__sxnQsEscape;
  delete globalThis.__sxnQsUnescape;

  // ---------------- node:url ----------------
  const url = {
    URL: globalThis.URL,
    URLSearchParams: globalThis.URLSearchParams,
    // Native (js_file_url_to_path in src/node.c): a scheme check, a host
    // check and a percent-decode, none of which needs a regexp.
    fileURLToPath: (u) => __sxnFileUrlToPath(typeof u === "string" ? u : String(u)),
    // The text is built native (js_path_to_file_url); the URL object it is
    // handed to is the engine's own.
    pathToFileURL: (p) => new URL(__sxnPathToFileUrl(String(p))),
    format: (u) => String(u),
    parse: (s) => { try { return new URL(s); } catch { return null; } },
  };
  globalThis.__sxnUrl = url;
  // ---------------- the builtins past the first twenty ----------------
  // These are built on first use rather than at startup. Every program pays
  // for what is above this line; almost none of them require node:dgram, and
  // constructing all of this eagerly cost about 4% of a cold start.
  const laterBuiltins = () => {
    // ---------------- node:child_process ----------------
    // One native primitive (js_spawn_sync in src/network.c) runs a child to
    // completion on a loop of its own. The synchronous calls are that call; the
    // asynchronous ones are that call plus the events Node emits afterwards, so
    // a child does not overlap with the rest of the program the way it does in
    // Node. Anything that streams a long-running child's output as it arrives
    // will see it all at once, at the end.
    function spawnArgs(command, args, options) {
      if (!Array.isArray(args)) { options = args; args = []; }
      return [args || [], options || {}];
    }
    function shellRun(command, options) {
      const shell = (options && typeof options.shell === "string" && options.shell) ||
        (process.platform === "win32" ? "cmd.exe" : "/bin/sh");
      const flag = process.platform === "win32" ? "/d/s/c" : "-c";
      return __sxnSpawnSync(shell, [flag, command], options || {});
    }
    function decodeOut(raw, options) {
      const encoding = options && options.encoding;
      if (encoding === "buffer" || encoding === null) return Buffer.from(raw);
      return new TextDecoder().decode(raw);
    }
    function spawnError(result, command) {
      const e = new Error("spawnSync " + command + " " + (result.errno || "failed"));
      e.code = result.errno || "UNKNOWN";
      e.syscall = "spawnSync " + command;
      e.path = command;
      return e;
    }
    function spawnSync(command, args, options) {
      const [list, opts] = spawnArgs(command, args, options);
      const raw = opts.shell ? shellRun([command].concat(list).join(" "), opts)
                             : __sxnSpawnSync(command, list, opts);
      const out = {
        pid: raw.pid || 0,
        status: raw.status === undefined ? null : raw.status,
        signal: raw.signal === undefined ? null : raw.signal,
        stdout: decodeOut(raw.stdout, opts),
        stderr: decodeOut(raw.stderr, opts),
        error: raw.error ? spawnError(raw, command) : undefined,
      };
      out.output = [null, out.stdout, out.stderr];
      return out;
    }
    function checkedSync(result, command) {
      if (result.error) throw result.error;
      if (result.status !== 0) {
        const e = new Error("Command failed: " + command + "\n" +
                            (typeof result.stderr === "string" ? result.stderr : ""));
        e.status = result.status;
        e.signal = result.signal;
        e.stdout = result.stdout;
        e.stderr = result.stderr;
        throw e;
      }
      return result.stdout;
    }
    // The asynchronous forms: run the child, then deliver its output and its
    // exit through the same events Node uses, one tick later.
    function ChildProcess(result) {
      EE.call(this);
      this.pid = result.pid || 0;
      this.exitCode = result.status === undefined ? null : result.status;
      this.signalCode = result.signal || null;
      this.killed = false;
      this.stdin = new Writable({ write(c, e, cb) { cb(); } });
      this.stdout = new Readable({ read() {} });
      this.stderr = new Readable({ read() {} });
    }
    inheritEE(ChildProcess);
    ChildProcess.prototype.kill = function () { this.killed = true; return false; };
    ChildProcess.prototype.unref = function () { return this; };
    ChildProcess.prototype.ref = function () { return this; };
    function deliver(child, raw, opts, cb) {
      process.nextTick(() => {
        if (raw.error) {
          child.emit("error", spawnError(raw, "child"));
          if (cb) cb(spawnError(raw, "child"), decodeOut(raw.stdout, opts), decodeOut(raw.stderr, opts));
          return;
        }
        if (raw.stdout.length) child.stdout.push(Buffer.from(raw.stdout));
        if (raw.stderr.length) child.stderr.push(Buffer.from(raw.stderr));
        child.stdout.push(null);
        child.stderr.push(null);
        if (cb) {
          const stdout = decodeOut(raw.stdout, opts), stderr = decodeOut(raw.stderr, opts);
          let e = null;
          if (raw.status !== 0) {
            e = new Error("Command failed");
            e.code = raw.status;
          }
          cb(e, stdout, stderr);
        }
        child.emit("exit", raw.status === undefined ? null : raw.status, raw.signal || null);
        child.emit("close", raw.status === undefined ? null : raw.status, raw.signal || null);
      });
      return child;
    }
    const childProcess = {
      spawnSync,
      execFileSync(file, args, options) {
        const [list, opts] = spawnArgs(file, args, options);
        return checkedSync(spawnSync(file, list, opts), file);
      },
      execSync(command, options) {
        const opts = options || {};
        const raw = shellRun(command, opts);
        return checkedSync({
          error: raw.error ? spawnError(raw, command) : undefined,
          status: raw.status, signal: raw.signal,
          stdout: decodeOut(raw.stdout, opts), stderr: decodeOut(raw.stderr, opts),
        }, command);
      },
      spawn(command, args, options) {
        const [list, opts] = spawnArgs(command, args, options);
        const raw = opts.shell ? shellRun([command].concat(list).join(" "), opts)
                               : __sxnSpawnSync(command, list, opts);
        return deliver(new ChildProcess(raw), raw, opts, null);
      },
      exec(command, options, cb) {
        if (typeof options === "function") { cb = options; options = {}; }
        const opts = options || {};
        const raw = shellRun(command, opts);
        return deliver(new ChildProcess(raw), raw, opts, cb);
      },
      execFile(file, args, options, cb) {
        if (typeof args === "function") { cb = args; args = []; options = {}; }
        else if (typeof options === "function") { cb = options; options = {}; }
        const opts = options || {};
        const raw = __sxnSpawnSync(file, args || [], opts);
        return deliver(new ChildProcess(raw), raw, opts, cb);
      },
      fork() { throw new Error("child_process.fork is not supported: a child would need its own runtime"); },
      ChildProcess,
    };
    globalThis.__sxnChildProcess = childProcess;

    // ---------------- node:dns ----------------
    // uv_getaddrinfo, resolved on this thread (js_dns_lookup in src/network.c).
    // The callback forms hand the answer back on a later tick, so they compose
    // like Node's, but the resolution itself blocks. There is no resolver
    // beyond the system one: the record types libuv cannot answer throw.
    function dnsLookup(hostname, options, cb) {
      if (typeof options === "function") { cb = options; options = {}; }
      const opts = typeof options === "number" ? { family: options } : (options || {});
      let list, error = null;
      try { list = __sxnDnsLookup(hostname, opts.family || 0); }
      catch (e) { error = e; list = []; }
      process.nextTick(() => {
        if (error) return cb(error);
        if (!list.length) {
          const e = new Error("getaddrinfo ENOTFOUND " + hostname);
          e.code = "ENOTFOUND";
          return cb(e);
        }
        if (opts.all) return cb(null, list);
        cb(null, list[0].address, list[0].family);
      });
    }
    function dnsResolve(family) {
      return function (hostname, cb) {
        dnsLookup(hostname, { family, all: true }, (e, list) =>
          e ? cb(e) : cb(null, list.map((a) => a.address)));
      };
    }
    function noResolver(kind) {
      return function (hostname, cb) {
        const e = new Error("dns.resolve" + kind + " is not supported: there is no resolver beyond the system one");
        e.code = "ENOTIMP";
        if (cb) return process.nextTick(() => cb(e));
        throw e;
      };
    }
    const dns = {
      lookup: dnsLookup,
      resolve4: dnsResolve(4),
      resolve6: dnsResolve(6),
      resolve(hostname, type, cb) {
        if (typeof type === "function") { cb = type; type = "A"; }
        if (type === "A") return dns.resolve4(hostname, cb);
        if (type === "AAAA") return dns.resolve6(hostname, cb);
        return noResolver(type)(hostname, cb);
      },
      resolveMx: noResolver("Mx"), resolveTxt: noResolver("Txt"),
      resolveSrv: noResolver("Srv"), resolveNs: noResolver("Ns"),
      resolveCname: noResolver("Cname"), reverse: noResolver("Ptr"),
      getServers: () => [],
      setServers() { throw new Error("dns.setServers is not supported: resolution goes through the system resolver"); },
      ADDRCONFIG: 1024, V4MAPPED: 8, ALL: 16,
    };
    const promisify1 = (fn) => (...a) => new Promise((res, rej) =>
      fn(...a, (e, v) => e ? rej(e) : res(v)));
    dns.promises = {
      lookup: (hostname, options) => new Promise((res, rej) =>
        dnsLookup(hostname, options || {}, (e, address, family) =>
          e ? rej(e) : res(typeof address === "string" ? { address, family } : address))),
      resolve4: promisify1(dns.resolve4),
      resolve6: promisify1(dns.resolve6),
      resolve: promisify1(dns.resolve),
      getServers: dns.getServers,
    };
    dns.Resolver = function Resolver() { return Object.assign(Object.create(null), dns); };
    globalThis.__sxnDns = dns;
    globalThis.__sxnDnsPromises = dns.promises;

    // ---------------- node:https ----------------
    // The same client node:http uses: the request goes out through the same
    // native fetch, which speaks TLS. Only the default protocol differs.
    // There is no https server, because Sxn.serve does not terminate TLS.
    const https = {
      request(options, cb) {
        const opts = typeof options === "string" ? { url: options } : Object.assign({}, options);
        if (!opts.url && !opts.protocol) opts.protocol = "https:";
        return http.request(opts, cb);
      },
      get(options, cb) { const r = https.request(options, cb); r.end(); return r; },
      Agent: function Agent(options) { this.options = options || {}; },
      globalAgent: null,
      createServer() { throw new Error("https.createServer is not supported: this runtime does not terminate TLS"); },
      Server: function Server() { throw new Error("https.Server is not supported: this runtime does not terminate TLS"); },
    };
    https.globalAgent = new https.Agent({});
    globalThis.__sxnHttps = https;

    // ---------------- node:tls / node:http2 ----------------
    // Named so that a `require` resolves and a feature check can fail cleanly,
    // rather than dying on a missing module. TLS is only available as a client,
    // through fetch and node:https; there is no socket to hand back.
    const tls = {
      connect() { throw new Error("tls.connect is not supported: use fetch or node:https"); },
      createServer() { throw new Error("tls.createServer is not supported: this runtime does not terminate TLS"); },
      TLSSocket: function TLSSocket() { throw new Error("tls.TLSSocket is not supported"); },
      Server: function Server() { throw new Error("tls.Server is not supported"); },
      createSecureContext: (options) => Object.assign({}, options),
      rootCertificates: [],
      DEFAULT_MIN_VERSION: "TLSv1.2",
      DEFAULT_MAX_VERSION: "TLSv1.3",
    };
    globalThis.__sxnTls = tls;

    const http2 = {
      constants: {
        HTTP2_HEADER_METHOD: ":method", HTTP2_HEADER_PATH: ":path",
        HTTP2_HEADER_STATUS: ":status", HTTP2_HEADER_AUTHORITY: ":authority",
        HTTP2_HEADER_SCHEME: ":scheme", HTTP2_HEADER_CONTENT_TYPE: "content-type",
      },
      connect() { throw new Error("http2.connect is not supported: the client speaks HTTP/1.1"); },
      createServer() { throw new Error("http2.createServer is not supported: the server speaks HTTP/1.1"); },
      createSecureServer() { throw new Error("http2.createSecureServer is not supported: the server speaks HTTP/1.1"); },
      getDefaultSettings: () => ({}),
    };
    globalThis.__sxnHttp2 = http2;

    // ---------------- node:stream/web ----------------
    // The Web Streams already in the global scope, under the names Node also
    // publishes them under. Nothing is reimplemented here.
    globalThis.__sxnStreamWeb = {
      ReadableStream: globalThis.ReadableStream,
      ReadableStreamDefaultReader: globalThis.ReadableStreamDefaultReader,
      ReadableStreamBYOBReader: globalThis.ReadableStreamBYOBReader,
      ReadableStreamDefaultController: globalThis.ReadableStreamDefaultController,
      ReadableByteStreamController: globalThis.ReadableByteStreamController,
      ReadableStreamBYOBRequest: globalThis.ReadableStreamBYOBRequest,
      WritableStream: globalThis.WritableStream,
      WritableStreamDefaultWriter: globalThis.WritableStreamDefaultWriter,
      WritableStreamDefaultController: globalThis.WritableStreamDefaultController,
      TransformStream: globalThis.TransformStream,
      TransformStreamDefaultController: globalThis.TransformStreamDefaultController,
      ByteLengthQueuingStrategy: globalThis.ByteLengthQueuingStrategy,
      CountQueuingStrategy: globalThis.CountQueuingStrategy,
      TextEncoderStream: globalThis.TextEncoderStream,
      TextDecoderStream: globalThis.TextDecoderStream,
      CompressionStream: globalThis.CompressionStream,
      DecompressionStream: globalThis.DecompressionStream,
    };

    // ---------------- node:vm ----------------
    // The engine has one realm, so a "new context" is a function whose
    // parameters are the sandbox's keys: the code sees those names, and writes
    // to them come back out. It is not an isolated global.
    const vm = {
      runInThisContext(code, options) {
        return (0, eval)(String(code));
      },
      runInNewContext(code, sandbox, options) {
        const box = sandbox || {};
        const keys = Object.keys(box);
        const fn = new Function(...keys, '"use strict"; return (' + "function(){" + String(code) + "}" + ")()");
        return fn(...keys.map((k) => box[k]));
      },
      runInContext(code, contextifiedObject, options) {
        return vm.runInNewContext(code, contextifiedObject, options);
      },
      createContext: (sandbox) => sandbox || {},
      isContext: (o) => typeof o === "object" && o !== null,
      compileFunction(code, params, options) {
        return new Function(...(params || []), String(code));
      },
      Script: function Script(code) {
        this.code = String(code);
        this.runInThisContext = () => vm.runInThisContext(this.code);
        this.runInNewContext = (sandbox) => vm.runInNewContext(this.code, sandbox);
        this.runInContext = (sandbox) => vm.runInNewContext(this.code, sandbox);
      },
    };
    globalThis.__sxnVm = vm;

    // ---------------- node:v8 ----------------
    // The numbers come from the engine's own allocator, not V8's, and the names
    // are Node's. serialize/deserialize are JSON, which covers the plain data
    // packages put through them and rejects what it cannot carry.
    const v8 = {
      getHeapStatistics() {
        const m = Sxn.memoryUsage();
        return {
          total_heap_size: m.mallocSize, used_heap_size: m.memoryUsed,
          heap_size_limit: m.mallocSize, total_available_size: 0,
          total_heap_size_executable: 0, total_physical_size: m.mallocSize,
          malloced_memory: m.mallocSize, peak_malloced_memory: m.mallocSize,
          does_zap_garbage: 0, number_of_native_contexts: 1, number_of_detached_contexts: 0,
        };
      },
      getHeapSpaceStatistics: () => [],
      setFlagsFromString() {},
      serialize(value) { return Buffer.from(JSON.stringify(value), "utf8"); },
      deserialize(buf) { return JSON.parse(Buffer.from(buf).toString("utf8")); },
      cachedDataVersionTag: () => 0,
    };
    globalThis.__sxnV8 = v8;

    // ---------------- node:worker_threads / node:cluster ----------------
    // One JS thread, one process. Both modules answer the questions a library
    // asks before it decides whether it is the main one -- which is most of
    // what they are used for -- and throw where a second thread is required.
    const workerThreads = {
      isMainThread: true,
      threadId: 0,
      parentPort: null,
      workerData: null,
      resourceLimits: {},
      SHARE_ENV: Symbol("nodejs.worker_threads.SHARE_ENV"),
      Worker: function Worker() { throw new Error("worker_threads.Worker is not supported: this runtime has one JS thread"); },
      MessageChannel: globalThis.MessageChannel,
      MessagePort: globalThis.MessagePort,
      BroadcastChannel: function BroadcastChannel() { throw new Error("BroadcastChannel is not supported: this runtime has one JS thread"); },
      markAsUntransferable() {},
      moveMessagePortToContext() { throw new Error("moveMessagePortToContext is not supported"); },
      receiveMessageOnPort: () => undefined,
      setEnvironmentData() {},
      getEnvironmentData: () => undefined,
    };
    globalThis.__sxnWorkerThreads = workerThreads;

    function Cluster() { EE.call(this); }
    inheritEE(Cluster);
    const cluster = new Cluster();
    cluster.isPrimary = true;
    cluster.isMaster = true;
    cluster.isWorker = false;
    cluster.worker = null;
    cluster.workers = {};
    cluster.settings = {};
    cluster.schedulingPolicy = 2;
    cluster.setupPrimary = function () {};
    cluster.setupMaster = function () {};
    cluster.fork = function () { throw new Error("cluster.fork is not supported: this runtime does not fork"); };
    cluster.disconnect = function (cb) { if (cb) process.nextTick(cb); };
    globalThis.__sxnCluster = cluster;

    // ---------------- node:readline ----------------
    // Lines out of any readable stream, and the promise form of question().
    // Terminal editing -- history, completion, cursor keys -- is not here:
    // stdin arrives as plain bytes.
    function Interface(options) {
      EE.call(this);
      const opts = options || {};
      this.input = opts.input || process.stdin;
      this.output = opts.output || process.stdout;
      this.terminal = false;
      this.closed = false;
      this._pending = [];
      this._rest = "";
      const self = this;
      if (this.input && typeof this.input.on === "function") {
        this.input.on("data", (chunk) => self._feed(String(chunk)));
        this.input.on("end", () => self._end());
        if (typeof this.input.resume === "function") this.input.resume();
      }
    }
    inheritEE(Interface);
    Interface.prototype._feed = function (text) {
      const parts = (this._rest + text).split("\n");
      this._rest = parts.pop();
      for (const line of parts) {
        const clean = line.endsWith("\r") ? line.slice(0, -1) : line;
        const waiter = this._pending.shift();
        if (waiter) waiter(clean);
        else this.emit("line", clean);
      }
    };
    Interface.prototype._end = function () {
      if (this._rest) { this._feed("\n"); }
      this.close();
    };
    Interface.prototype.question = function (query, cb) {
      if (this.output && typeof this.output.write === "function") this.output.write(query);
      this._pending.push(cb);
    };
    Interface.prototype.prompt = function () {};
    Interface.prototype.write = function (text) {
      if (this.output && typeof this.output.write === "function") this.output.write(text);
    };
    Interface.prototype.setPrompt = function () {};
    Interface.prototype.pause = function () { return this; };
    Interface.prototype.resume = function () { return this; };
    Interface.prototype.close = function () {
      if (this.closed) return;
      this.closed = true;
      this.emit("close");
    };
    Interface.prototype[Symbol.asyncIterator] = function () {
      const lines = [];
      let waiting = null, done = false;
      this.on("line", (l) => { if (waiting) { const w = waiting; waiting = null; w({ value: l, done: false }); } else lines.push(l); });
      this.on("close", () => { done = true; if (waiting) { const w = waiting; waiting = null; w({ value: undefined, done: true }); } });
      return {
        next() {
          if (lines.length) return Promise.resolve({ value: lines.shift(), done: false });
          if (done) return Promise.resolve({ value: undefined, done: true });
          return new Promise((res) => { waiting = res; });
        },
        [Symbol.asyncIterator]() { return this; },
      };
    };
    const readline = {
      Interface,
      createInterface: (options, output) =>
        new Interface(options && options.read !== undefined ? { input: options, output } : options),
      clearLine: () => true, clearScreenDown: () => true,
      cursorTo: () => true, moveCursor: () => true,
      emitKeypressEvents() {},
      promises: null,
    };
    readline.promises = {
      Interface,
      createInterface(options, output) {
        const rl = readline.createInterface(options, output);
        const ask = rl.question.bind(rl);
        rl.question = (query) => new Promise((res) => ask(query, res));
        return rl;
      },
    };
    globalThis.__sxnReadline = readline;
    globalThis.__sxnReadlinePromises = readline.promises;

    // ---------------- node:async_hooks ----------------
    // AsyncLocalStorage is real and is the reason this module is here: a store
    // entered for a synchronous run, and kept across an await by binding it to
    // the promise chain the callback returns. The hook API around it reports
    // one execution context, because that is what a single loop with no async
    // tracking can honestly say.
    function AsyncLocalStorage() { this._store = undefined; this._entered = false; }
    AsyncLocalStorage.prototype.run = function (store, callback, ...args) {
      const previous = this._store, wasIn = this._entered;
      this._store = store;
      this._entered = true;
      let async = false;
      const restore = () => { this._store = previous; this._entered = wasIn; };
      try {
        const out = callback(...args);
        // A callback that returns a promise keeps the store until the promise
        // settles. Anything else that runs while it is awaiting sees the store
        // too, which is where this parts company with Node: there is no async
        // context tracking underneath, only the promise chain handed back.
        if (out && typeof out.then === "function") {
          async = true;
          return out.then((v) => { restore(); return v; }, (e) => { restore(); throw e; });
        }
        return out;
      } finally {
        if (!async) restore();
      }
    };
    AsyncLocalStorage.prototype.exit = function (callback, ...args) {
      return this.run(undefined, callback, ...args);
    };
    AsyncLocalStorage.prototype.getStore = function () { return this._entered ? this._store : undefined; };
    AsyncLocalStorage.prototype.enterWith = function (store) { this._store = store; this._entered = true; };
    AsyncLocalStorage.prototype.disable = function () { this._store = undefined; this._entered = false; };
    function AsyncResource(type) { this.type = type; }
    AsyncResource.prototype.runInAsyncScope = function (fn, thisArg, ...args) { return fn.apply(thisArg, args); };
    AsyncResource.prototype.emitDestroy = function () { return this; };
    AsyncResource.prototype.asyncId = function () { return 1; };
    AsyncResource.prototype.triggerAsyncId = function () { return 0; };
    AsyncResource.bind = (fn) => fn;
    const asyncHooks = {
      AsyncLocalStorage, AsyncResource,
      executionAsyncId: () => 1,
      triggerAsyncId: () => 0,
      executionAsyncResource: () => ({}),
      createHook: () => ({ enable() { return this; }, disable() { return this; } }),
    };
    globalThis.__sxnAsyncHooks = asyncHooks;

    // ---------------- node:inspector ----------------
    // There is no debug protocol behind this. It exists so that a library can
    // ask whether a session is open and get "no" instead of a crash.
    const inspector = {
      url: () => undefined,
      open() { throw new Error("inspector.open is not supported: this runtime has no debug protocol"); },
      close() {},
      waitForDebugger() { throw new Error("inspector.waitForDebugger is not supported"); },
      console: globalThis.console,
      Session: function Session() { throw new Error("inspector.Session is not supported: this runtime has no debug protocol"); },
    };
    inspector.promises = { Session: inspector.Session };
    globalThis.__sxnInspector = inspector;

    // ---------------- node:dgram ----------------
    // A real UDP socket (uv_udp_t, in src/network.c) with the EventEmitter
    // shape Node gives it. Multicast is not wired up.
    function Socket(options) {
      EE.call(this);
      this.type = (options && (options.type || options)) === "udp6" ? "udp6" : "udp4";
      this._port = 0;
      this._handle = __sxnUdpOpen(this.type === "udp6", (bytes, address, port) => {
        this.emit("message", Buffer.from(bytes), { address, port, family: this.type === "udp6" ? "IPv6" : "IPv4", size: bytes.length });
      });
    }
    inheritEE(Socket);
    Socket.prototype.bind = function (port, address, cb) {
      if (typeof port === "object" && port !== null) { address = port.address; port = port.port; }
      if (typeof address === "function") { cb = address; address = undefined; }
      this._port = __sxnUdpBind(this._handle, Number(port) || 0, address);
      if (cb) this.once("listening", cb);
      process.nextTick(() => this.emit("listening"));
      return this;
    };
    Socket.prototype.send = function (data, port, address, cb) {
      if (typeof address === "function") { cb = address; address = undefined; }
      const bytes = typeof data === "string" ? Buffer.from(data, "utf8")
        : ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
        : new Uint8Array(data);
      let error = null, sent = 0;
      try { sent = __sxnUdpSend(this._handle, bytes, Number(port) || 0, address); }
      catch (e) { error = e; }
      if (cb) process.nextTick(() => cb(error, sent));
      else if (error) process.nextTick(() => this.emit("error", error));
      return this;
    };
    Socket.prototype.address = function () {
      return { address: this.type === "udp6" ? "::" : "0.0.0.0", port: this._port, family: this.type === "udp6" ? "IPv6" : "IPv4" };
    };
    Socket.prototype.close = function (cb) {
      __sxnUdpClose(this._handle);
      if (cb) this.once("close", cb);
      process.nextTick(() => this.emit("close"));
      return this;
    };
    Socket.prototype.ref = function () { return this; };
    Socket.prototype.unref = function () { return this; };
    Socket.prototype.setBroadcast = function () { return this; };
    const dgram = {
      Socket,
      createSocket(options, listener) {
        const s = new Socket(options);
        if (typeof options === "object" && options && typeof listener !== "function") listener = options.listener;
        if (typeof listener === "function") s.on("message", listener);
        return s;
      },
    };
    globalThis.__sxnDgram = dgram;

    // ---------------- node:console / node:constants ----------------
    // Both are the older shape of things that live elsewhere now: the global
    // console, and the constants that hang off fs, os and crypto.
    globalThis.__sxnConsole = Object.assign(Object.create(null), globalThis.console, {
      Console: function Console() { return globalThis.console; },
    });
    globalThis.__sxnConstants = Object.assign({}, fs.constants, os.constants, {
      SIGINT: 2, SIGTERM: 15, SIGKILL: 9,
    });

    // ---------------- node:punycode ----------------
    // The algorithm from RFC 3492, which is small enough to be worth having
    // rather than a stub: `url` used to need it and some packages still do.
    const punyBase = 36, punyTMin = 1, punyTMax = 26, punySkew = 38,
          punyDamp = 700, punyInitialBias = 72, punyInitialN = 128;
    function punyAdapt(delta, numPoints, firstTime) {
      delta = firstTime ? Math.floor(delta / punyDamp) : delta >> 1;
      delta += Math.floor(delta / numPoints);
      let k = 0;
      while (delta > ((punyBase - punyTMin) * punyTMax) >> 1) {
        delta = Math.floor(delta / (punyBase - punyTMin));
        k += punyBase;
      }
      return k + Math.floor(((punyBase - punyTMin + 1) * delta) / (delta + punySkew));
    }
    function punyDecode(input) {
      const output = [];
      const basic = input.lastIndexOf("-");
      let n = punyInitialN, bias = punyInitialBias, i = 0;
      for (let j = 0; j < (basic < 0 ? 0 : basic); j++) output.push(input.charCodeAt(j));
      for (let index = basic < 0 ? 0 : basic + 1; index < input.length;) {
        const oldi = i;
        for (let w = 1, k = punyBase;; k += punyBase) {
          const code = input.charCodeAt(index++);
          const digit = code - 48 < 10 ? code - 22 : code - 65 < 26 ? code - 65 : code - 97 < 26 ? code - 97 : punyBase;
          if (digit >= punyBase) throw new RangeError("Invalid input");
          i += digit * w;
          const t = k <= bias ? punyTMin : k >= bias + punyTMax ? punyTMax : k - bias;
          if (digit < t) break;
          w *= punyBase - t;
        }
        bias = punyAdapt(i - oldi, output.length + 1, oldi === 0);
        n += Math.floor(i / (output.length + 1));
        i %= output.length + 1;
        output.splice(i++, 0, n);
      }
      return String.fromCodePoint(...output);
    }
    function punyEncode(input) {
      const points = Array.from(input).map((c) => c.codePointAt(0));
      const basic = points.filter((c) => c < 128);
      const output = basic.map((c) => String.fromCharCode(c));
      let handled = basic.length;
      if (handled) output.push("-");
      let n = punyInitialN, delta = 0, bias = punyInitialBias;
      while (handled < points.length) {
        let m = Infinity;
        for (const c of points) if (c >= n && c < m) m = c;
        delta += (m - n) * (handled + 1);
        n = m;
        for (const c of points) {
          if (c < n) delta++;
          else if (c === n) {
            let q = delta;
            for (let k = punyBase;; k += punyBase) {
              const t = k <= bias ? punyTMin : k >= bias + punyTMax ? punyTMax : k - bias;
              if (q < t) break;
              output.push(String.fromCharCode(punyDigit(t + ((q - t) % (punyBase - t)))));
              q = Math.floor((q - t) / (punyBase - t));
            }
            output.push(String.fromCharCode(punyDigit(q)));
            bias = punyAdapt(delta, handled + 1, handled === basic.length);
            delta = 0;
            handled++;
          }
        }
        delta++;
        n++;
      }
      return output.join("");
    }
    const punyDigit = (d) => d + 22 + (d < 26 ? 75 : 0);
    const mapDomain = (text, fn) => text.split(".").map(fn).join(".");
    const punycode = {
      encode: punyEncode,
      decode: punyDecode,
      toASCII: (text) => mapDomain(text, (part) =>
        /[^\x00-\x7F]/.test(part) ? "xn--" + punyEncode(part) : part),
      toUnicode: (text) => mapDomain(text, (part) =>
        part.startsWith("xn--") ? punyDecode(part.slice(4)) : part),
      ucs2: {
        decode: (text) => Array.from(text).map((c) => c.codePointAt(0)),
        encode: (points) => String.fromCodePoint(...points),
      },
      version: "2.3.1",
    };
    globalThis.__sxnPunycode = punycode;

    // ---------------- node:diagnostics_channel ----------------
    // Named channels with subscribers, which is all of it that does not depend
    // on async context tracking.
    const channels = new Map();
    function Channel(name) { this.name = name; this._subscribers = []; }
    Object.defineProperty(Channel.prototype, "hasSubscribers", {
      get() { return this._subscribers.length > 0; },
    });
    Channel.prototype.publish = function (message) {
      for (const fn of this._subscribers.slice()) {
        try { fn(message, this.name); } catch { /* a subscriber must not break the publisher */ }
      }
    };
    Channel.prototype.subscribe = function (fn) { this._subscribers.push(fn); };
    Channel.prototype.unsubscribe = function (fn) {
      const i = this._subscribers.indexOf(fn);
      if (i < 0) return false;
      this._subscribers.splice(i, 1);
      return true;
    };
    Channel.prototype.bindStore = function () {};
    Channel.prototype.runStores = function (message, fn, thisArg, ...args) {
      this.publish(message);
      return fn.apply(thisArg, args);
    };
    function channelFor(name) {
      let c = channels.get(name);
      if (!c) { c = new Channel(name); channels.set(name, c); }
      return c;
    }
    const diagnosticsChannel = {
      Channel,
      channel: channelFor,
      hasSubscribers: (name) => channels.has(name) && channels.get(name).hasSubscribers,
      subscribe: (name, fn) => channelFor(name).subscribe(fn),
      unsubscribe: (name, fn) => channelFor(name).unsubscribe(fn),
      tracingChannel(name) {
        return {
          start: channelFor(name + ":start"), end: channelFor(name + ":end"),
          asyncStart: channelFor(name + ":asyncStart"), asyncEnd: channelFor(name + ":asyncEnd"),
          error: channelFor(name + ":error"),
          traceSync(fn, context, thisArg, ...args) { return fn.apply(thisArg, args); },
          tracePromise(fn, context, thisArg, ...args) { return fn.apply(thisArg, args); },
          traceCallback(fn, position, context, thisArg, ...args) { return fn.apply(thisArg, args); },
        };
      },
    };
    globalThis.__sxnDiagnosticsChannel = diagnosticsChannel;
  };
  // The names above, each a getter that builds the whole group once and then
  // gets out of the way -- the group is one closure, so splitting it further
  // would buy nothing.
  let builtLater = false;
  for (const name of ["__sxnChildProcess", "__sxnDns", "__sxnDnsPromises", "__sxnHttps", "__sxnTls", "__sxnHttp2", "__sxnStreamWeb", "__sxnVm", "__sxnV8", "__sxnWorkerThreads", "__sxnCluster", "__sxnReadline", "__sxnReadlinePromises", "__sxnAsyncHooks", "__sxnInspector", "__sxnDgram", "__sxnConsole", "__sxnConstants", "__sxnPunycode", "__sxnDiagnosticsChannel"]) {
    Object.defineProperty(globalThis, name, {
      configurable: true,
      get() {
        if (!builtLater) {
          builtLater = true;
          for (const other of ["__sxnChildProcess", "__sxnDns", "__sxnDnsPromises", "__sxnHttps", "__sxnTls", "__sxnHttp2", "__sxnStreamWeb", "__sxnVm", "__sxnV8", "__sxnWorkerThreads", "__sxnCluster", "__sxnReadline", "__sxnReadlinePromises", "__sxnAsyncHooks", "__sxnInspector", "__sxnDgram", "__sxnConsole", "__sxnConstants", "__sxnPunycode", "__sxnDiagnosticsChannel"]) delete globalThis[other];
          laterBuiltins();
        }
        return globalThis[name];
      },
      set(value) {
        Object.defineProperty(globalThis, name, { value, writable: true, configurable: true });
      },
    });
  }

})();
