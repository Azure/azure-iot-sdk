# vcpkg overlay ports

`openssl`: OpenSSL 4.1.0. vcpkg has no 4.x port yet; this is its `openssl` port
([microsoft/vcpkg](https://github.com/microsoft/vcpkg), MIT License) at the
`builtin-baseline` of `c/vcpkg.json`, updated to 4.1.0:

- Version and source hash.
- Patches rebased onto 4.1.0: `windows/install-layout.patch`, `windows/install-pdbs.patch`,
  `unix/no-empty-dirs.patch`, `unix/no-static-libs-for-shared.patch`.
- Dropped: `script-prefix.patch` and the `c_rehash` install steps (4.x has no `c_rehash`), the
  `ssl3` feature (4.x rejects `enable-ssl3`), and the `enable-static-engine` and
  `enable-capieng` options (4.x has no ENGINE).
- Added: `configure-no-apps-out-of-source.patch`, openssl/openssl@6bef602005, which 4.1.0 lacks.

Use it with `-DVCPKG_OVERLAY_PORTS=<repo>/c/eng/vcpkg-ports`. CI's `installed-deps-windows` job
builds against it in addition to the baseline's 3.6. Remove it when vcpkg ships OpenSSL 4.x.
