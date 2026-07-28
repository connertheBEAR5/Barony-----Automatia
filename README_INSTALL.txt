BARONY DIALOGUE EDITOR PART 13 DECLARATION FIX
=============================================

Cause
-----
The previous package placed the forward declaration after three earlier
functions had already called the helper.

Correct verified source order
-----------------------------
Forward declaration: line 3411
First helper call:   line 3416
Function definition: line 3616

The required order is:

    declaration
    calls
    definition

Install
-------

    cd ~/Barony-----Automatia

    unzip -o ~/Downloads/barony_dialogue_editor_guided_actions_part13_declaration_fix.zip \
        -d ~/Barony-----Automatia

Verify ordering:

    grep -n "questDialogueEditorEnsureChoiceAction" src/editor.cpp

The first result must end with a semicolon and appear before all calls.

Check formatting:

    grep -n '\\t' src/editor.cpp

That command should print nothing.

Build the editor first:

    cmake --build build --target editor -j1 \
        2>&1 | tee dialogue-editor-guided-actions-part13-declaration-fix.log

After the editor passes, build everything:

    cmake --build build -j1 \
        2>&1 | tee full-build-after-part13-fix.log
