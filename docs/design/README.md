# Wapi IDE Design QA

## Visual truth
- Source: `docs/design/forge-grid-ops.png`
- Source viewport: 1488 x 1056
- Implementation: native Tauri window capture, Computer Use `screenshot-0`
- Implementation viewport: 1280 x 820
- Compared state: loaded `project-walrus` workspace with Runtime Inspector open

## Comparison
The source visual and native implementation were placed together in one comparison input. The implementation preserves the black and chartreuse Forge Grid hierarchy, condensed utility typography, left activity rail, project explorer, runtime policy strip, editor/tool layout, status bar, and inspector concealed behind the top toolbar.

A focused toolbar and Runtime Inspector pass confirmed the Check/Run hierarchy, visible capability grants, process and memory sections, live events, close control, and responsive fit at the smaller native viewport.

## Findings resolved
- Restored the chartreuse Check action after legacy selector specificity muted it.
- Added an accessible label and expanded state to the Runtime Inspector control.
- Verified the inspector compacts over the work area without clipping at 1280 x 820.
- Removed all capture-only project data and restored the required blank explorer and closed inspector on startup.
- Verified the clean first-launch surface in the native Tauri window.

## Final result
passed
