import { Buffer } from "node:buffer";
console.log(Buffer.from("cold start", "utf-8").toString("hex"));
setTimeout(() => console.log("timer done"), 1);
