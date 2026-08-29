// discord_krisp.node の N-API 側からモデル一覧などを取得する（解析補助）
const path = require('path');
const dir = path.join(process.env.LOCALAPPDATA,
  'Discord', 'app-1.0.9255', 'modules', 'discord_krisp-1', 'discord_krisp');
const m = require(path.join(dir, 'discord_krisp.node'));

m._initialize(undefined);
console.log('sdkVersion       :', JSON.stringify(m.getSdkVersion()));
console.log('suppressionLevel :', m.getSuppressionLevel());
m._getNcModels(x => console.log('ncModels         :', JSON.stringify(x)));
m._getVadModels(x => console.log('vadModels        :', JSON.stringify(x)));
m._getNcModelFilename(x => console.log('ncModelFilename  :', JSON.stringify(x)));
setTimeout(() => process.exit(0), 1500);
