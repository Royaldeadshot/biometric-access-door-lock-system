const fs = require('fs');
const sch = fs.readFileSync('C:\\CODING\\biometric access door lock system\\Wiring\\biometric access door lock.kicad_sch', 'utf8');

const symRegex = /\(symbol\s+\(lib_id\s+"LED:SK6812"\)\s+\(at\s+([\d\.\-]+)\s+([\d\.\-]+)\s+([\d\.\-]+)\)[\s\S]*?\(property\s+"Reference"\s+"(D\d+)"/g;
let sm;
const leds = [];
while ((sm = symRegex.exec(sch)) !== null) {
  leds.push({ ref: sm[4], x: parseFloat(sm[1]), y: parseFloat(sm[2]) });
}

leds.sort((a,b) => a.x - b.x);
console.log("LED chain order from left to right:");
leds.forEach((l, idx) => console.log(`Position ${idx + 1}: ${l.ref} at x=${l.x}`));
