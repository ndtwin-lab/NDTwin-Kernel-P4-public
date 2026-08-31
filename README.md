# NDTwin-Kernel-P4

The NDTwin Kernel with P4/BMv2 data-plane support: a P4 proxy agent that presents Ryu's
northbound interface over P4Runtime, the `ndtwin_switch.p4` pipeline, and the Mininet
topologies that stand a BMv2 fabric up.

It is what the [NDTwin installation manual](https://ndtwin.org/docs/ndtwin-installation-manual/)
means when a page says you need the P4 build of the kernel. If a page you are following
mentions P4, BMv2, `p4c` or `simple_switch_grpc`, this is the tree to clone; otherwise use
[`NDTwin-Kernel`](https://github.com/ndtwin-lab/NDTwin-Kernel).

## What this repository is

**A snapshot, not a mirror.** It is the state of the P4 development tree at a point in
time, published as a single commit. There is no upstream history here and nothing updates
it automatically — a refresh means taking another snapshot deliberately.

| | |
|---|---|
| Snapshot taken | 2026-09-01 |
| Source revision | `20cd80b` (2026-08-28) |
| Not included | the internal engineering audit tree (`doc/audit/`), which is development record rather than anything you need to build or run this |

If you would rather not build at all, the P4/BMv2 demo VM on the
[download page](https://ndtwin.org/docs/download/) already has this installed and working.

## Known rough edges

**Some scripts and docs hardcode a developer's home directory.** Paths of the form
`/home/adam/...` appear as defaults in the files below. None of them are secrets; they are
simply not your paths, so treat them as values to change rather than as instructions:

```
intelligent_router.py
p4_proxy/requirements.txt
p4_proxy/mininet/ntg_bmv2_topo.py          (NTG_DIR is overridable: export NTG_DIR=...)
p4_proxy/reference/p4runtime_mastership_probe.py
p4_proxy/reference/clone_stack_probe.py
p4_proxy/tests/test_clone_session.py
tools/test_workflow/  (several)
doc/  (several runbooks quote them in example commands)
```

`doc/KNOWN-ISSUES.md` is the list of defects known at snapshot time and is worth reading
before you file one.

[Co-developed with claude code -- Adam]

---

Branch for developing P4 support.
