exports.name = "a";
const b = require("./cyclic-b.cjs");
exports.sawB = b.name;
