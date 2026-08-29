import { EventEmitter } from "node:events";

const ee = new EventEmitter();
let seen = 0;
const first = (v) => { seen += v; };
const second = (v) => { seen += v * 2; };

ee.on("x", first);
if (ee._events.x !== first) throw new Error("singleton listener was not stored directly");
if (ee.listenerCount("x") !== 1 || ee.listeners("x")[0] !== first) throw new Error("singleton introspection failed");
if (!ee.emit("x", 3) || seen !== 3) throw new Error("singleton emit failed");

ee.on("x", second);
if (!Array.isArray(ee._events.x) || ee.listenerCount("x") !== 2) throw new Error("listener promotion failed");
ee.emit("x", 4);
if (seen !== 15) throw new Error("promoted emit failed");
ee.off("x", first);
if (ee.listenerCount("x") !== 1 || ee._events.x !== second) throw new Error("listener demotion failed");
ee.emit("x", 5);
if (seen !== 25) throw new Error("demoted emit failed");

const changing = new EventEmitter();
changing.on("x", () => changing.removeAllListeners("x"));
changing.emit("x");
if (changing.listenerCount("x") !== 0) throw new Error("mutation during emit failed");

const onceEe = new EventEmitter();
const onceFn = () => {};
onceEe.once("x", onceFn);
if (onceEe.listeners("x")[0] !== onceFn) throw new Error("once listener introspection failed");
onceEe.off("x", onceFn);
if (onceEe.listenerCount("x") !== 0) throw new Error("once listener removal failed");
