#!/usr/bin/env python3
"""Fetch pinned optional native-parser references into the ignored cache.

No downloaded code is executed. Package SHA-512 and selected assembly SHA-256
come from the reviewed source lock; original package/license metadata remains
in the cache. The native parser is a separate optional audit tool, not an app
dependency.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import io
import json
from pathlib import Path
from pathlib import PurePosixPath
import zipfile


def collector_module():
    spec = importlib.util.spec_from_file_location('grammar_collector',Path(__file__).with_name('collect-sql-grammar.py'))
    collector = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(collector)
    return collector


def collect_sqlserver(args,source):
    if not source['url'].startswith('https://api.nuget.org/v3-flatcontainer/'):
        raise ValueError('expected the reviewed official NuGet package URL')
    args.cache.mkdir(parents=True,exist_ok=True)
    package = args.cache / (source['package']+'.'+source['version']+'.nupkg')
    content = package.read_bytes() if package.is_file() else b''
    def matches(data):
        return base64.b64encode(hashlib.sha512(data).digest()).decode()==source['package_sha512_base64']
    if not matches(content):
        content = collector_module().fetch(source['url'],max_bytes=40*1024*1024)
        if not matches(content):
            raise ValueError('official package SHA-512 does not match the source lock')
        temporary = package.with_suffix('.partial')
        temporary.write_bytes(content)
        temporary.replace(package)
    archive = zipfile.ZipFile(io.BytesIO(content))
    assembly = archive.read(source['assembly_path'])
    if hashlib.sha256(assembly).hexdigest()!=source['assembly_sha256']:
        raise ValueError('native assembly SHA-256 does not match the source lock')
    library = args.cache/'lib'/Path(source['assembly_path']).name
    library.parent.mkdir(exist_ok=True)
    temporary = library.with_suffix('.partial')
    temporary.write_bytes(assembly)
    temporary.replace(library)
    for name in archive.namelist():
        filename = Path(name).name
        if filename.lower() in ('license','license.txt','license.md','readme.md') or name.endswith('.nuspec'):
            (args.cache/filename).write_bytes(archive.read(name))
    (args.cache/'metadata.json').write_text(json.dumps(source,indent=2)+'\n')
    print(f"Verified {source['package']} {source['version']}: {library}")


def collect_postgresql(args,source):
    if not source['url'].startswith('https://files.pythonhosted.org/packages/'):
        raise ValueError('expected the reviewed PyPI artifact URL')
    args.cache.mkdir(parents=True,exist_ok=True)
    package = args.cache/source['filename']
    content = package.read_bytes() if package.is_file() else b''
    if hashlib.sha256(content).hexdigest()!=source['sha256']:
        content = collector_module().fetch(source['url'],max_bytes=20*1024*1024)
        if hashlib.sha256(content).hexdigest()!=source['sha256'] or len(content)!=source['bytes']:
            raise ValueError('pglast wheel hash/size does not match the source lock')
        temporary = package.with_suffix('.partial')
        temporary.write_bytes(content)
        temporary.replace(package)
    archive = zipfile.ZipFile(io.BytesIO(content))
    if hashlib.sha256(archive.read(source['native_path'])).hexdigest()!=source['native_sha256']:
        raise ValueError('PostgreSQL native parser does not match the source lock')
    entries = archive.infolist()
    if len(entries)>1024 or sum(info.file_size for info in entries)>64*1024*1024:
        raise ValueError('native parser archive exceeds extraction limits')
    site = args.cache/'site-packages'
    for info in entries:
        relative = PurePosixPath(info.filename)
        if relative.is_absolute() or '..' in relative.parts or '\\' in info.filename or \
            not relative.parts or relative.parts[0] not in ('pglast',f"pglast-{source['version']}.dist-info") or \
            (info.external_attr>>16)&0o170000==0o120000:
            raise ValueError('unexpected native parser archive path')
        if info.is_dir():
            continue
        path = site.joinpath(*relative.parts)
        path.parent.mkdir(parents=True,exist_ok=True)
        temporary = path.with_name(path.name+'.partial')
        temporary.write_bytes(archive.read(info))
        temporary.replace(path)
    (args.cache/'metadata.json').write_text(json.dumps(source,indent=2)+'\n')
    print(f"Verified pglast {source['version']} ({source['python_tag']}, {source['platform_tag']}): {site}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lock',type=Path,default=Path('tests/sql/corpus/reference-sources.json'))
    parser.add_argument('--engine',choices=['sqlserver','postgresql'],default='sqlserver')
    parser.add_argument('--cache',type=Path)
    args = parser.parse_args()
    if args.cache is None:
        args.cache = Path(f'.cache/{args.engine}-native-parser')
    source = json.loads(args.lock.read_text())['native_parser_tools'][args.engine]
    (collect_sqlserver if args.engine=='sqlserver' else collect_postgresql)(args,source)


if __name__=='__main__':
    main()
