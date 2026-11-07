# Security policy

The project's [GitHub Releases page](https://github.com/mpkfresh/File-Manager-1.0/releases)
identifies any supported File Manager release. Source checkouts and unsigned
review artifacts are development materials; do not distribute them as a
supported 1.0 release. Private vulnerability reporting is enabled for this
public source repository. The planned 1.0 MSI and portable ZIP have no
publisher signature. Source, redistribution rights, exact-package checks and
acceptance must pass before release approval. A review artifact does not
establish 1.0 acceptance.

The [Code signing policy](https://github.com/mpkfresh/File-Manager-1.0/blob/main/CODE_SIGNING_POLICY.md) names the owner-approved
source, review and signing roles; no signing request has been made.

## Dependency and response policy

The exact dependency inventory, review cadence and remediation deadlines are tracked during
development; major components and immutable source revisions appear in
`THIRD_PARTY_NOTICES.md`. Critical or known-exploited vulnerabilities are release blockers. High
severity fixes are targeted within 14 days of an upstream correction, and medium severity issues
within 30 days or the next planned release, whichever comes first. Security updates are reviewed one
dependency family at a time and must pass the exact-byte Release regression suite, including its
malformed and untrusted-input cases, plus package and clean-machine acceptance before publication.

Full built-in file-content previews and image thumbnails run outside the main process. On Windows
the preview broker passes one read-only inherited source handle to a zero-capability AppContainer
and applies kill-on-close, one-process and 768 MiB Job Object limits. There is no unsandboxed
production fallback for those previews. The current working development build also sends image
dimensions and PDF, RTF and Office document metadata through handle-only modes of that restricted
helper; affected development checks and the hosted V1 build passed on this
source. Folder previews count entries in the main process. Registered
third-party Shell metadata and context handlers remain behind independent
default-off permissions. Exact-byte package, native and installed acceptance
for the final 1.0 release remains open.

File Manager has no automatic updater. Security fixes for a supported release
will be distributed as complete reviewed packages from the official release
page. Any unsigned package must be clearly labeled; a future signed build
requires separate review. File Manager never downloads or executes an update
in the background.

## Reporting

Do not attach private filenames, paths, document contents, crash dumps or unredacted diagnostics to
a public issue. For a private report, use this repository's Security → Advisories →
Report a vulnerability control. GitHub exposes that form publicly. Redacted diagnostic JSON can be
reviewed before sharing; paths are structurally absent unless the separate default-off inclusion
control is explicitly enabled.
