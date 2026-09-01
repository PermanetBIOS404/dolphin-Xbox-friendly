# Dolphin RWiN — Follow-up Patch Notes

## Wii Export Assistant completion dialog

Status: Known minor UI issue; not a Linux release blocker.

During the packaged Linux release-candidate smoke test with:

- Game: Wii Sports + Wii Sports Resort
- ID6: SP2E01
- Source format: NKit v1
- d2x playable repair enabled
- Test destination: 06C-SP2E01-Release-Smoke-Test

the export completed successfully.

The final Wii Export Assistant success/completion dialog displays the complete output path, but
the path is too large for the current dialog width and wraps awkwardly across multiple lines.

The underlying path is correct and the export succeeded, so this is a presentation-only issue.

Planned small follow-up patch:

- Improve long output-path presentation in the final successful-export confirmation dialog.
- Keep the complete path available to the user.
- Preferably make it easy to select and copy.
- Avoid allowing a long path to make the confirmation dialog visually cramped.
- Keep the patch narrowly scoped to this completion-dialog UI issue.

The exact Qt solution has not been chosen and is intentionally deferred to the follow-up patch.

### Validation note

The packaged Linux release candidate:

- Reached the d2x repair path correctly.
- Regenerated the Wii partition hash hierarchy and TMD content digest.
- Completed the SP2E01 WBFS export successfully.
- Displayed the expected warning that Nintendo-original signature authenticity is not preserved.
