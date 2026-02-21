var fs = require('fs');
if (process.argv.length !== 4) {
  console.error('Usage: node comparejson.js <file1.json> <file2.json>');
  process.exit(2);
}
try {
  var a = JSON.stringify(JSON.parse(fs.readFileSync(process.argv[2], "utf8")));
  var b = JSON.stringify(JSON.parse(fs.readFileSync(process.argv[3], "utf8")));
  if (a !== b) {
    console.log(process.argv[2] + " and " + process.argv[3] + " are not equivalent");
    process.exit(1);
  }
  process.exit(0);
} catch (e) {
  console.error(e.message || e);
  process.exit(2);
}
