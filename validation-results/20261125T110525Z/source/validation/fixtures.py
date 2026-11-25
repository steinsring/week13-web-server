"""검증 입력은 고정 바이트다. 생성기·백엔드가 같은 원본과 해시를 사용한다."""
import hashlib
import json
from pathlib import Path

SMALL = (b'LJM static validation\n' * 200)[:4096].ljust(4096, b'.')
LARGE = bytes(range(256)) * 4096
API_GET = (b'backend deterministic GET\n' * 50)[:1024].ljust(1024, b'g')
API_POST = bytes(reversed(range(256))) * 4


def prepare(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    values = {'small.txt': SMALL, 'large.bin': LARGE, 'api-get.bin': API_GET, 'api-post.bin': API_POST}
    manifest = {}
    for name, value in values.items():
        (directory / name).write_bytes(value)
        manifest[name] = {'length': len(value), 'sha256': hashlib.sha256(value).hexdigest()}
    (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    return manifest
