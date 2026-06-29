const fs = require('fs');

const batchStr = `{"type":"telemetry","session_time":1.1}\n{"type":"motion","session_time":1.1}\n`;
const dupLines = [`{"type":"status","session_time":1.1}`, `{"type":"lap","session_time":1.1}`];

const finalBatch = dupLines.length > 0 
  ? batchStr + (batchStr.endsWith('\n') ? '' : '\n') + dupLines.join('\n') 
  : batchStr;

console.log("FINAL BATCH:\n" + finalBatch);

let start = 0;
while (start < finalBatch.length) {
  let end = finalBatch.indexOf('\n', start);
  if (end === -1) end = finalBatch.length;
  if (end > start) {
    try {
      const raw = JSON.parse(finalBatch.slice(start, end));
      console.log("PARSED:", raw.type);
    } catch (e) {
      console.error('Failed to parse batch JSON:', e, "STRING WAS:", finalBatch.slice(start, end));
    }
  }
  start = end + 1;
}
