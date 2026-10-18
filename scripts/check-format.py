"""只约束 fork 改动的代码行，保留上游格式和预编译数组。"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

from clang_format import get_executable, git_clang_format

BASELINE = '973761621353b99bee3dc7d4bb27b117fef2644f'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fix', action='store_true', help='格式化改动行，保留到工作区供审阅')
    args = parser.parse_args()
    os.chdir(Path(__file__).resolve().parents[1])
    binary = get_executable('clang-format')
    version = subprocess.check_output([binary, '--version'], text=True).strip()
    if version != 'clang-format version 20.1.8':
        parser.error('需要 clang-format 20.1.8，请安装 scripts/requirements-format.txt')
    if subprocess.run(['git', 'merge-base', '--is-ancestor', BASELINE, 'HEAD']).returncode:
        parser.error('缺少固定上游祖先，请取得完整 Git 历史；不能用当前 HEAD 代替检查基线')
    print(f'{version}; baseline={BASELINE}', flush=True)
    sys.argv = ['git-clang-format', '--binary', binary, '--extensions', 'c,h,cpp,hpp,cc,cxx,hxx']
    if args.fix:
        # --fix 明确授权格式化当前工作区内容；LLVM 以当前内容构造临时树，不丢弃已有代码。
        sys.argv.append('--force')
    else:
        sys.argv.append('--diff')
    sys.argv += [BASELINE, '--', 'OptiScaler', 'tests/menu-localization',
                 ':(exclude)OptiScaler/include/**',
                 ':(exclude)OptiScaler/shaders/**/precompile/**']
    result = git_clang_format.main()
    # LLVM 的应用模式用 1 表示确实写入了格式修正；检查模式仍将差异作为失败返回。
    return 0 if args.fix and result == 1 else result


if __name__ == '__main__':
    sys.exit(main())
