# Code signing policy

File Manager 1.0 is in release review. No signed 1.0 package has been
published. If the project is accepted for free open-source signing, the
release page will identify the exact signed files and their SHA-256 values.

Free code signing provided by [SignPath.io](https://about.signpath.io/),
certificate by [SignPath Foundation](https://signpath.org/terms.html).
This is the proposed signing route; it does not claim that SignPath Foundation
has accepted this project or signed an artifact.

## Roles

- Source maintainer and committer: [`mpkfresh`](https://github.com/mpkfresh).
- Reviewer for proposed changes from outside contributors: `mpkfresh`.
- Approver of each signing request: `mpkfresh`.

GitHub MFA is enabled for the source maintainer. Any SignPath account used
for signing must also use MFA before a signing request.

The source maintainer reviews source and build-script changes before a
release build. The signing approver checks the reviewed source commit,
dependency and notice inventory, unsigned artifact hashes, hosted build
evidence and open security issues before approving a specific signing
request. Signing input must come from a GitHub-hosted build of the reviewed
source; a local binary upload is not accepted as signing input.

Only File Manager's own executable files and installer are candidates for
this project's signature. Bundled Qt, FFmpeg and other upstream files retain
their own publishers and licenses; they are not individually signed with this
project's certificate.

After signing, File Manager checks the resulting signatures, exact hashes,
clean Windows installation and the planned acceptance cases before the owner
approves a stable release. An unsigned review prerelease is labeled as such
and does not establish signing trust or final 1.0 acceptance.

## Privacy and security

See the [privacy policy](https://github.com/mpkfresh/File-Manager-1.0/blob/main/PRIVACY.md)
for local data and network behavior. Report vulnerabilities through the
repository's [private reporting form](https://github.com/mpkfresh/File-Manager-1.0/security/advisories/new).
The project does not transfer information to another networked system unless
the user or installer explicitly requests the action.
