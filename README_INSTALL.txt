BARONY QUEST JOURNAL TEXT VISIBILITY FIX
=========================================

cd ~/Barony-----Automatia

unzip -o ~/Downloads/barony_quest_journal_text_visibility_fix.zip \
    -d ~/Barony-----Automatia

cmake --build build --target barony -j1 \
    2>&1 | tee quest-journal-text-visibility-fix.log
