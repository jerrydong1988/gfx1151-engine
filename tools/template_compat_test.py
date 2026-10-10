"""Compare old/new template drivers byte-for-byte on the existing A/B corpus."""
import argparse
import json
from pathlib import Path
import subprocess
from template_ab import build_cases, build_fuzz, cli_req


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--baseline',required=True)
    parser.add_argument('--candidate',required=True)
    args=parser.parse_args()
    cases=build_cases()+build_fuzz(400)
    payload=''.join(json.dumps(cli_req(case),ensure_ascii=False)+'\n' for _,case in cases).encode('utf-8')
    def run(binary):
        result=subprocess.run([str(Path(binary).resolve())],input=payload,capture_output=True,check=True)
        return [json.loads(line) for line in result.stdout.splitlines()]
    old,new=run(args.baseline),run(args.candidate)
    assert len(old)==len(new)==len(cases),(len(old),len(new),len(cases))
    for (name,case),left,right in zip(cases,old,new):
        assert left==right,(name,case,left,right)
    print(f'RESULT PASS {len(cases)} renders/errors identical to baseline; defaults, tools, reasoning and malformed inputs included')


if __name__=='__main__':main()
