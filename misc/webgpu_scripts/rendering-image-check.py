from pathlib import Path
from PIL import Image,ImageStat,ImageChops
import json,sys
root=Path(sys.argv[1]);native=Path(sys.argv[2])
def mae(a,b):return sum(ImageStat.Stat(ImageChops.difference(a,b)).mean)/3
baseline=Image.open(root/'baseline.png').convert('RGB');report={}
for p in root.glob('*.png'):
 im=Image.open(p).convert('RGB');stats=ImageStat.Stat(im)
 ref=Image.open(native/('coverage-'+p.name)).convert('RGB')
 row={'mean':stats.mean,'stddev':stats.stddev,'baseline_mae':mae(im,baseline),'native_mae':mae(im,ref)};report[p.stem]=row
 assert min(stats.stddev)>5,(p,'blank image',row)
 if p.stem.startswith('fsr'):assert row['baseline_mae']<15,(p,'upscaler output diverged',row)
 if p.stem in ['baseline','fsr1','fsr2','fsr2_resize']:assert row['native_mae']<15,(p,'native comparison diverged',row)
assert report['fog']['baseline_mae']>1,'Fog produced no visible effect'
(root/'image-metrics.json').write_text(json.dumps(report,indent=2))
print('PASS image validation',root)
