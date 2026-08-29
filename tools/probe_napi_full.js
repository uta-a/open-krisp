// napi経由で完全初期化+モデル設定し、モデル一覧を取得する。
const path=require('path');
const dir=path.join(process.env.LOCALAPPDATA,'Discord','app-1.0.9255','modules','discord_krisp-1','discord_krisp');
process.chdir(dir);  // cwd をモジュール位置に
const m=require(path.join(dir,'discord_krisp.node'));
m._initialize(undefined);
console.log('sdk:',m.getSdkVersion());
m._getNcModels(models=>{
  console.log('ncModels:',JSON.stringify(models));
  m._getNcModelFilename(fn=>{
    console.log('currentNcModelFilename:',JSON.stringify(fn));
    console.log('suppressionLevel:',m.getSuppressionLevel());
    process.exit(0);
  });
});
setTimeout(()=>process.exit(0),2000);
