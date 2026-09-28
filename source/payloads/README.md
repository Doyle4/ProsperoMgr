# Bundled payloads

Third-party `.elf` payloads shipped inside ProsperoMgr, unmodified, and
launched through `PayloadRegistry` / `ElfldrClient` instead of being
re-implemented. `tools/gen_payloads.py` embeds every file here into the
backend ELF; `InstallBundledPayloads()` writes them to
`/data/ProsperoMgr/bundled/` on every startup, so they always match the
current build. ProsperoMgr never edits them after that.

| File | Source | Why it's here |
|---|---|---|
| `ftpsrv.elf` | [ps5-payload-dev/ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) | An FTP **server** for external clients (FileZilla, etc.) on 127.0.0.1:2121. The web Files tab uses native filesystem calls, not this. |
| `klogsrv.elf` | [ps5-payload-dev/klogsrv](https://github.com/ps5-payload-dev/klogsrv) | Forwards `/dev/klog` to TCP :3232 for console log viewing. |

These `.elf` binaries are the only thing checked in here — the upstream
source they're built from is not part of this repo. To rebuild one, grab
its source separately (e.g. `git clone https://github.com/ps5-payload-dev/ftpsrv`)
and build it with its own `Makefile.ps5`:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd /path/to/ftpsrv
make -f Makefile.ps5 ftpsrv-ps5.elf
cp ftpsrv-ps5.elf /path/to/ProsperoMgr/source/payloads/ftpsrv.elf
```

Then re-embed and rebuild from the project root: `make payloads && make`.
