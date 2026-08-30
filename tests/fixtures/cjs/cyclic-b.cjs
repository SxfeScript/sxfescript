const a = require("./cyclic-a.cjs");
exports.name = "b";
exports.sawA = a.name;   // a is only partially initialized here, as in Node
