"""Check INFO capacity negotiation with both legacy and current engines."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
from api_disconnect_test import free_port, make_tokenizer, wait_for
from api_regression_test import get, request


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--api',required=True)
    args=parser.parse_args()
    api=str(Path(args.api).resolve())
    fake=str(Path(__file__).with_name('api_fake_engine.py').resolve())
    for pool in [0,262144,524288]:
        children=[]
        with tempfile.TemporaryDirectory(prefix='gdec-capacity-health-') as temporary:
            root=Path(temporary)
            make_tokenizer(root/'tokenizer')
            ep,ap=free_port(),free_port()
            while ep==ap: ap=free_port()
            env={k:v for k,v in os.environ.items() if not k.startswith('GDEC_')}
            env.update(GDEC_API_TOKCACHE_FILE='',GDEC_REQSTAT='0',GDEC_API_MAX_IMAGES='32')
            flags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0
            with (root/'test.log').open('w+') as log:
                try:
                    children.append(subprocess.Popen([sys.executable,fake,'--port',str(ep),
                        '--slots','2','--pool',str(pool)],cwd=root,stdout=log,stderr=log,env=env,creationflags=flags))
                    def ready():
                        try:
                            with socket.create_connection(('127.0.0.1',ep),timeout=.1):return True
                        except OSError:return False
                    wait_for(ready,'engine')
                    children.append(subprocess.Popen([api,'--tokenizer',str(root/'tokenizer'),
                        '--engine',f'127.0.0.1:{ep}','--host','127.0.0.1','--port',str(ap),
                        '--context','262144','--overrides',''],cwd=root,stdout=log,stderr=log,env=env,creationflags=flags))
                    base=f'http://127.0.0.1:{ap}'
                    def healthy():
                        try:return get(base,'/health')[0]==200
                        except OSError:return False
                    wait_for(healthy,'API')
                    health=json.loads(get(base,'/health')[2])
                    assert health['kv_pool']==pool and health['slots']==2, health
                    assert health['vision']['max_images']==32,health
                    assert health['slot_ctx']==262144,health
                    result=request(base,'/v1/completions',{'prompt':'x','max_tokens':2,'temperature':0})
                    assert result[0]==200,result
                    print(f'PASS pool={pool} slots=2 per_slot=262144 images=32 generation=200')
                except BaseException:
                    log.flush();log.seek(0);print(log.read());raise
                finally:
                    for child in reversed(children):
                        if child.poll() is None:child.terminate()
                        child.wait(timeout=10)
    print('RESULT PASS')


if __name__=='__main__':main()
