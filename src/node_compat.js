/* Minimal `node:*` compatibility layer, layered the same way bootstrap.js
   layers WinterCG globals: pure spec/behavior logic lives here in JS, native
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
  // directly. `once` stays JS: its self-removing wrapper has no natural
  // native shape (it needs to reference itself to call `self.off(type,
  // wrapped)`), and it's not a hot path the way emit() is.
  function EventEmitter() {
    this._events = Object.create(null);
  }
  EventEmitter.prototype.on = __sxnEeOn;
  EventEmitter.prototype.addListener = __sxnEeOn;
  EventEmitter.prototype.once = function (type, listener) {
    var self = this;
    function wrapped() {
      self.off(type, wrapped);
      listener.apply(this, arguments);
    }
    wrapped._original = listener;
    return this.on(type, wrapped);
  };
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
  delete globalThis.__sxnEeOff;
  delete globalThis.__sxnEeEmit;
  delete globalThis.__sxnEeListenerCount;
  delete globalThis.__sxnEeListeners;
  delete globalThis.__sxnEeRemoveAllListeners;

  // ---------------- buffer: Buffer ----------------
  // Only called for non-utf-8 encodings -- Buffer.from's string branch
  // handles utf-8 itself via a native primitive that skips the extra
  // allocation this returns-a-bare-Uint8Array shape would otherwise cost.
  // `encoding` arrives already-lowercased from that call site.
  // Node decodes as much as it can and stops, where the standard
  // Uint8Array.fromHex throws. Take the strict, native path first and fall
  // back only when it refuses, so valid input keeps its speed.
  function hexBytesLenient(str) {
    var out = new Uint8Array(str.length >> 1), n = 0;
    for (var i = 0; i + 1 < str.length; i += 2) {
      var hi = hexDigit(str.charCodeAt(i) & 0xff), lo = hexDigit(str.charCodeAt(i + 1) & 0xff);
      if (hi < 0 || lo < 0) break;
      out[n++] = (hi << 4) | lo;
    }
    return out.subarray(0, n);
  }
  function hexDigit(c) {
    if (c >= 48 && c <= 57) return c - 48;
    if (c >= 97 && c <= 102) return c - 87;
    if (c >= 65 && c <= 70) return c - 55;
    return -1;
  }

  // Node's base64 reader takes either alphabet, skips anything that is not a
  // base64 character, and does not require padding.
  var B64 = (function () {
    var t = new Int8Array(128).fill(-1);
    var a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (var i = 0; i < a.length; i++) t[a.charCodeAt(i)] = i;
    t[43] = 62; t[47] = 63;   // + /
    t[45] = 62; t[95] = 63;   // - _  (base64url)
    return t;
  })();
  function base64BytesLenient(str) {
    var out = new Uint8Array((str.length * 3) >> 2), n = 0, acc = 0, bits = 0;
    for (var i = 0; i < str.length; i++) {
      // Node reads the string one byte at a time, so a code unit above 0xff is
      // truncated rather than skipped -- which is why an emoji ends a base64
      // string: the high half of its surrogate pair masks down to '='.
      var c = str.charCodeAt(i) & 0xff;
      if (c === 61) break;                        // '=' ends the data
      var v = c < 128 ? B64[c] : -1;
      if (v < 0) continue;
      acc = (acc << 6) | v; bits += 6;
      if (bits >= 8) { bits -= 8; out[n++] = (acc >> bits) & 0xff; }
    }
    return out.subarray(0, n);
  }

  function utf16leBytes(str) {
    var out = new Uint8Array(str.length * 2);
    for (var i = 0; i < str.length; i++) {
      var c = str.charCodeAt(i);
      out[i * 2] = c & 0xff;
      out[i * 2 + 1] = c >> 8;
    }
    return out;
  }
  function utf16leString(bytes) {
    var out = "";
    for (var i = 0; i + 1 < bytes.length; i += 2)
      out += String.fromCharCode(bytes[i] | (bytes[i + 1] << 8));
    return out;
  }

  function bufferBytesFromString(str, encoding) {
    if (encoding === "hex") {
      try { return Uint8Array.fromHex(str); } catch { return hexBytesLenient(str); }
    }
    if (encoding === "base64" || encoding === "base64url") {
      try {
        return encoding === "base64" ? Uint8Array.fromBase64(str)
                                     : Uint8Array.fromBase64(str, { alphabet: "base64url" });
      } catch { return base64BytesLenient(str); }
    }
    if (encoding === "ucs2" || encoding === "ucs-2" ||
        encoding === "utf16le" || encoding === "utf-16le") return utf16leBytes(str);
    if (encoding === "latin1" || encoding === "binary" || encoding === "ascii") {
      var bytes = new Uint8Array(str.length);
      for (var i = 0; i < str.length; i++) bytes[i] = str.charCodeAt(i) & 0xff;
      return bytes;
    }
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
      if (encoding === "latin1" || encoding === "binary") {
        var out = "";
        for (var i = 0; i < this.length; i++) out += String.fromCharCode(this[i]);
        return out;
      }
      if (encoding === "ascii") {
        // Node's "ascii" is 7-bit: the high bit is stripped, unlike latin1.
        var a = "";
        for (var j = 0; j < this.length; j++) a += String.fromCharCode(this[j] & 0x7f);
        return a;
      }
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
      if (data instanceof ArrayBuffer) return new Buffer(data); // zero-copy view over the whole buffer
      if (ArrayBuffer.isView(data)) return new Buffer(data.buffer, data.byteOffset, data.byteLength); // zero-copy view
      if (Array.isArray(data) || (data && typeof data.length === "number")) return new Buffer(data); // copies, matching Node
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

    static concat(list, totalLength) {
      if (totalLength === undefined) {
        totalLength = 0;
        for (var i = 0; i < list.length; i++) totalLength += list[i].length;
      }
      var out = new Buffer(totalLength);
      var offset = 0;
      for (var j = 0; j < list.length && offset < totalLength; j++) {
        var chunk = list[j].subarray(0, Math.min(list[j].length, totalLength - offset));
        out.set(chunk, offset);
        offset += chunk.length;
      }
      return out;
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
  process.cwd = function () { return __sxnCwd(); };
  process.exit = function (code) { __sxnExit(code === undefined ? 0 : code); };
  // A genuine job-queue microtask (queueMicrotask is itself a thin JS_EnqueueJob
  // wrapper built into quickjs.c), not a timer -- so nextTick callbacks always
  // run before any subsequently-scheduled timer/I-O callback, matching Node's
  // "runs before the event loop continues" contract for the cases this runtime
  // supports.
  process.nextTick = function (fn) {
    var args = Array.prototype.slice.call(arguments, 1);
    queueMicrotask(function () { fn.apply(undefined, args); });
  };

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
  function Stats(raw) {
    for (var key in raw) this[key] = raw[key];
    this.atime = new Date(raw.atimeMs);
    this.mtime = new Date(raw.mtimeMs);
    this.ctime = new Date(raw.ctimeMs);
    this.birthtime = new Date(raw.birthtimeMs);
  }
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
    statSync: function (path) { return new Stats(__sxnStat(path, true)); },
    lstatSync: function (path) { return new Stats(__sxnStat(path, false)); },
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
  globalThis.__sxnFs = fs;
  delete globalThis.__sxnWriteFileSync;
  delete globalThis.__sxnExistsSync;

  var fsPromises = {
    readFile: function (path, encoding) {
      return Sxn.file(path).text().then(function (text) {
        return wantsText(encoding) ? text : Buffer.from(new TextEncoder().encode(text).buffer);
      });
    },
    writeFile: __sxnWriteFileAsync,
    stat: function (path) {
      try { return Promise.resolve(new Stats(__sxnStat(path, true))); }
      catch (e) { return Promise.reject(e); }
    },
    lstat: function (path) {
      try { return Promise.resolve(new Stats(__sxnStat(path, false))); }
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
    return this._buf.length < 16;      // a coarse high-water mark
  };
  function drainReadable(r) {
    while (r._flowing && r._buf.length > 0) r.emit("data", r._buf.shift());
    maybeEndReadable(r);
    if (r._flowing && !r._ended) r._read();
  }
  function maybeEndReadable(r) {
    if (r._ended && r._buf.length === 0 && !r._endEmitted) {
      r._endEmitted = true;
      r.readable = false;
      queueMicrotask(() => { r.emit("end"); r.emit("close"); });
    }
  }
  Readable.prototype.read = function () {
    if (this._buf.length === 0) { this._read(); }
    if (this._buf.length === 0) { maybeEndReadable(this); return null; }
    let c;
    if (this._readableObjectMode || this._encoding || this._buf.length === 1) {
      c = this._buf.shift();
    } else {
      // Node hands back everything buffered as one Buffer.
      c = Buffer.concat(this._buf.splice(0));
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
  Readable.prototype.pipe = function (dest, options) {
    const onData = (chunk) => { if (dest.write(chunk) === false) this.pause(); };
    const onDrain = () => this.resume();
    const onEnd = () => { if (!options || options.end !== false) dest.end(); };
    const onError = (e) => dest.emit("error", e);
    this.on("data", onData);
    dest.on("drain", onDrain);
    this.on("end", onEnd);
    this.on("error", onError);
    (this._pipes || (this._pipes = [])).push({ dest, onData, onDrain, onEnd, onError });
    this.resume();
    return dest;
  };
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
        if (self._buf.length > 0) return Promise.resolve({ value: self._buf.shift(), done: false });
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
    let sync = true, backpressure = false;
    this._write(chunk, enc || "utf8", (err) => {
      if (err) { this.emit("error", err); if (cb) cb(err); return; }
      if (cb) cb(null);
      if (backpressure) queueMicrotask(() => this.emit("drain"));
    });
    sync = false; void sync;
    return !backpressure;
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
    this.headers = {};
    for (const k of Object.keys(raw.headers || {}))
      this.headers[k.toLowerCase()] = raw.headers[k];
    this.httpVersion = "1.1";
    this.rawHeaders = [];
    for (const k of Object.keys(this.headers)) this.rawHeaders.push(k, this.headers[k]);
    // on-finished reads socket.readable and `complete` to decide whether a
    // request is spent; body-parser skips parsing when it says yes, so both
    // have to describe a request whose body has not been read yet. The socket
    // is a real emitter because on-finished subscribes to its 'error' and
    // 'close', and a plain object had no on() for it to call.
    this.socket = Object.assign(new EE(), {
      remoteAddress: "127.0.0.1", remotePort: 0, localAddress: "127.0.0.1",
      encrypted: false, readable: true, writable: true, destroyed: false,
      setTimeout() { return this; }, setNoDelay() { return this; },
      setKeepAlive() { return this; },
      destroy() { this.destroyed = true; this.readable = this.writable = false;
                  this.emit("close"); return this; },
      end() { return this.destroy(); },
    });
    this.connection = this.socket;
    this.complete = false;
    this.once("end", () => { this.complete = true; });
    // The body is already off the wire, but it must not be pushed before the
    // consumer attaches: body-parser adds its 'data' listener after the
    // handler returns, and an eagerly-ended stream would hand it nothing.
    // Pushing from _read defers until something actually reads.
    let sent = false;
    const body = raw.body;
    this._read = () => {
      if (sent) return;
      sent = true;
      if (body !== undefined && body !== null && body !== "") this.push(body);
      this.push(null);
    };
  }
  IncomingMessage.prototype = Object.create(Readable.prototype);
  IncomingMessage.prototype.constructor = IncomingMessage;

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
  ServerResponse.prototype.setHeader = function (name, value) {
    this._headers[String(name).toLowerCase()] = value;
    return this;
  };
  ServerResponse.prototype.getHeader = function (name) { return this._headers[String(name).toLowerCase()]; };
  ServerResponse.prototype.getHeaders = function () { return Object.assign({}, this._headers); };
  ServerResponse.prototype.getHeaderNames = function () { return Object.keys(this._headers); };
  ServerResponse.prototype.hasHeader = function (name) {
    return Object.prototype.hasOwnProperty.call(this._headers, String(name).toLowerCase());
  };
  ServerResponse.prototype.removeHeader = function (name) { delete this._headers[String(name).toLowerCase()]; };
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
    // Concatenate once: text chunks join, binary chunks merge into one array.
    let body;
    const anyBinary = this._chunks.some((c) => c instanceof Uint8Array || c instanceof ArrayBuffer);
    if (anyBinary) {
      const parts = this._chunks.map((c) =>
        c instanceof Uint8Array ? c
        : c instanceof ArrayBuffer ? new Uint8Array(c)
        : new TextEncoder().encode(String(c)));
      let total = 0; for (const p of parts) total += p.length;
      body = new Uint8Array(total);
      let at = 0; for (const p of parts) { body.set(p, at); at += p.length; }
    } else {
      body = this._chunks.map((c) => String(c)).join("");
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
      if (enc === "hex") {
        const out = new Uint8Array(d.length >> 1);
        for (let i = 0; i < out.length; i++) out[i] = parseInt(d.substr(i * 2, 2), 16);
        return out;
      }
      if (enc === "base64") return Uint8Array.from(atob(d), (c) => c.charCodeAt(0));
      return new TextEncoder().encode(d);
    }
    if (d instanceof ArrayBuffer) return new Uint8Array(d);
    if (ArrayBuffer.isView(d)) return new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
    throw new TypeError("expected a string, Buffer or TypedArray");
  };
  const encodeDigest = (bytes, encoding) => {
    if (!encoding || encoding === "buffer")
      return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (encoding === "hex")
      return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
    if (encoding === "base64")
      return btoa(String.fromCharCode(...bytes));
    throw new TypeError("unsupported digest encoding: " + encoding);
  };
  const concatBytes = (parts) => {
    let total = 0; for (const p of parts) total += p.length;
    const out = new Uint8Array(total);
    let at = 0; for (const p of parts) { out.set(p, at); at += p.length; }
    return out;
  };

  function Hash(algorithm) {
    this._algo = String(algorithm).toLowerCase();
    this._parts = [];
  }
  Hash.prototype.update = function (data, enc) {
    this._parts.push(cryptoToBytes(data, enc));
    return this;
  };
  Hash.prototype.digest = function (encoding) {
    return encodeDigest(__sxnDigest(this._algo, concatBytes(this._parts)), encoding);
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
    return encodeDigest(__sxnHmac(this._algo, this._key, concatBytes(this._parts)), encoding);
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
            let total = 0; for (const p of parts) total += p.length;
            const joined = new Uint8Array(total);
            let at = 0; for (const p of parts) { joined.set(p, at); at += p.length; }
            cb(null, syncFn(joined, options));
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
    this._dec = new TextDecoder(this.encoding === "utf8" ? "utf-8" : this.encoding);
  }
  StringDecoder.prototype.write = function (buf) {
    if (typeof buf === "string") return buf;
    return this._dec.decode(buf, { stream: true });
  };
  StringDecoder.prototype.end = function (buf) {
    let out = buf ? this.write(buf) : "";
    out += this._dec.decode();
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
    builtinModules: ["assert","buffer","events","fs","http","os","path","process",
                     "querystring","stream","string_decoder","timers","tty","url","util"],
    isBuiltin: (n) => moduleModule.builtinModules.includes(String(n).replace(/^node:/, "")),
  });
  globalThis.__sxnModule = moduleModule;

  // CommonJS reaches the builtins through require(), which is synchronous, so
  // it cannot go through import(). Every builtin is already a plain object on
  // a global; this maps a specifier to it, with or without the node: prefix.
  globalThis.__sxnBuiltinRequire = function (specifier) {
    const name = String(specifier).replace(/^node:/, "");
    const table = {
      buffer: { Buffer, default: Buffer },
      events: globalThis.__sxnEventEmitter,
      path: globalThis.__sxnPath,
      process: globalThis.process,
      fs: globalThis.__sxnFs,
      "fs/promises": globalThis.__sxnFsPromises,
      util: globalThis.__sxnUtil,
      os: globalThis.__sxnOs,
      url: globalThis.__sxnUrl,
      querystring: globalThis.__sxnQuerystring,
      assert: globalThis.__sxnAssert,
      "assert/strict": globalThis.__sxnAssert,
      stream: globalThis.__sxnStream,
      "stream/promises": globalThis.__sxnStream && globalThis.__sxnStream.promises,
      http: globalThis.__sxnHttp,
      tty: globalThis.__sxnTty,
      string_decoder: globalThis.__sxnStringDecoder,
      timers: globalThis.__sxnTimers,
      "timers/promises": globalThis.__sxnTimers && globalThis.__sxnTimers.promises,
      perf_hooks: globalThis.__sxnPerfHooks,
      module: globalThis.__sxnModule,
      zlib: globalThis.__sxnZlib,
      crypto: globalThis.__sxnCrypto,
      net: globalThis.__sxnNet,
    };
    const m = table[name];
    if (m === undefined) {
      const e = new Error("Cannot find module '" + specifier + "'");
      e.code = "MODULE_NOT_FOUND";
      throw e;
    }
    // events exports the constructor itself, and Node lets you reach the
    // named helpers off it either way.
    return m;
  };
  globalThis.__sxnIsBuiltin = function (specifier) {
    try { globalThis.__sxnBuiltinRequire(specifier); return true; } catch { return false; }
  };

  // ---------------- node:util ----------------
  // The parts packages actually import: promisify, callbackify, inherits,
  // format, deprecate, and the types guards. inspect is a readable
  // approximation, not Node's exact formatter.
  function inspect(v, opts, depth) {
    opts = opts || {};
    const max = opts.depth === undefined ? 2 : opts.depth;
    const seen = opts._seen || new Set();
    depth = depth || 0;
    const t = typeof v;
    if (v === null) return "null";
    if (t === "string") return depth === 0 && !opts.quoteStrings ? v : JSON.stringify(v);
    if (t === "number" || t === "boolean" || t === "undefined") return String(v);
    if (t === "bigint") return String(v) + "n";
    if (t === "symbol") return v.toString();
    if (t === "function") return "[Function: " + (v.name || "anonymous") + "]";
    if (v instanceof Error) return v.stack || (v.name + ": " + v.message);
    if (v instanceof Date) return v.toISOString();
    if (v instanceof RegExp) return String(v);
    if (seen.has(v)) return "[Circular *1]";
    if (depth > max) return Array.isArray(v) ? "[Array]" : "[Object]";
    seen.add(v);
    const sub = Object.assign({}, opts, { _seen: seen, quoteStrings: true });
    let out;
    if (Array.isArray(v)) {
      out = "[ " + v.map((e) => inspect(e, sub, depth + 1)).join(", ") + " ]";
      if (v.length === 0) out = "[]";
    } else if (v instanceof Map) {
      out = "Map(" + v.size + ") {" + (v.size ? " " + [...v].map(([k, val]) =>
        inspect(k, sub, depth + 1) + " => " + inspect(val, sub, depth + 1)).join(", ") + " " : "") + "}";
    } else if (v instanceof Set) {
      out = "Set(" + v.size + ") {" + (v.size ? " " + [...v].map((e) =>
        inspect(e, sub, depth + 1)).join(", ") + " " : "") + "}";
    } else if (ArrayBuffer.isView(v)) {
      out = v.constructor.name + "(" + v.length + ") [ " + Array.from(v).join(", ") + " ]";
    } else {
      const keys = Object.keys(v);
      out = keys.length === 0 ? "{}" : "{ " + keys.map((k) =>
        (/^[A-Za-z_$][\w$]*$/.test(k) ? k : JSON.stringify(k)) + ": " +
        inspect(v[k], sub, depth + 1)).join(", ") + " }";
    }
    seen.delete(v);
    return out;
  }

  function format(...args) {
    if (typeof args[0] !== "string") return args.map((a) => inspect(a)).join(" ");
    // With nothing to substitute, Node returns the string untouched -- even
    // "%%" stays as written.
    if (args.length === 1) return args[0];
    let i = 1;
    let out = args[0].replace(/%[sdifjoOc%]/g, (m) => {
      if (m === "%%") return "%";
      if (i >= args.length) return m;
      const a = args[i++];
      switch (m) {
        case "%s": return typeof a === "string" ? a : inspect(a);
        case "%d": case "%f": return typeof a === "bigint" ? a + "n" : Number(a).toString();
        case "%i": return typeof a === "bigint" ? a + "n" : parseInt(a, 10).toString();
        case "%j": try { return JSON.stringify(a); } catch { return "[Circular]"; }
        case "%o": case "%O": return inspect(a, { depth: 4 });
        case "%c": return "";
        default: return m;
      }
    });
    for (; i < args.length; i++) out += " " + (typeof args[i] === "string" ? args[i] : inspect(args[i]));
    return out;
  }

  const util = {
    inspect,
    format,
    // Node keeps the custom-inspect symbol here.
    promisify(fn) {
      if (typeof fn !== "function") throw new TypeError("promisify expects a function");
      const wrapped = function (...args) {
        return new Promise((resolve, reject) => {
          fn.call(this, ...args, (err, ...values) =>
            err ? reject(err) : resolve(values.length > 1 ? values : values[0]));
        });
      };
      Object.defineProperty(wrapped, "name", { value: fn.name, configurable: true });
      return wrapped;
    },
    callbackify(fn) {
      return function (...args) {
        const cb = args.pop();
        Promise.resolve(fn.apply(this, args)).then((v) => cb(null, v), (e) => cb(e || new Error("rejected")));
      };
    },
    inherits(ctor, superCtor) {
      Object.defineProperty(ctor, "super_", { value: superCtor, writable: true, configurable: true });
      Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
    },
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
  function deepEqual(a, b, strict) {
    if (strict ? Object.is(a, b) : a == b) return true;
    if (a === null || b === null || typeof a !== "object" || typeof b !== "object") {
      return strict ? Object.is(a, b) : a == b;
    }
    if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
    if (a instanceof Date) return a.getTime() === b.getTime();
    if (a instanceof RegExp) return String(a) === String(b);
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    if (ArrayBuffer.isView(a)) {
      if (a.length !== b.length) return false;
      for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
      return true;
    }
    if (a instanceof Map) {
      if (a.size !== b.size) return false;
      for (const [k, v] of a) { if (!b.has(k) || !deepEqual(v, b.get(k), strict)) return false; }
      return true;
    }
    if (a instanceof Set) {
      if (a.size !== b.size) return false;
      for (const v of a) if (!b.has(v)) return false;
      return true;
    }
    const ka = Object.keys(a), kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) {
      if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
      if (!deepEqual(a[k], b[k], strict)) return false;
    }
    return true;
  }

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
    fileURLToPath(u) {
      const s = typeof u === "string" ? u : String(u);
      if (!s.startsWith("file://")) throw new TypeError("must be a file: URL");
      return decodeURIComponent(s.slice(7).replace(/^localhost/, "")) || "/";
    },
    pathToFileURL(p) {
      return new URL("file://" + encodeURI(String(p)).replace(/[?#]/g, encodeURIComponent));
    },
    format: (u) => String(u),
    parse: (s) => { try { return new URL(s); } catch { return null; } },
  };
  globalThis.__sxnUrl = url;
})();
