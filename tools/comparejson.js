var fs = require('fs');
if (JSON.stringify(JSON.parse(fs.readFileSync(process.argv[2], "utf8"))) !== JSON.stringify(JSON.parse(fs.readFileSync(process.argv[3], "utf8")))) {
  console.log(process.argv[2] + " and " + process.argv[3] + " are not equivalent");
}
