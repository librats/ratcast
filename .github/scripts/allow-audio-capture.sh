#!/bin/sh
# The leave to hear other applications (a process tap, macOS 14.2) given where
# the runner's image gives the microphone's: asked for instead, nobody answers,
# and each tap waits three minutes to be refused. The rows are copied whole,
# whatever columns this macOS's table has.
for db in "$HOME/Library/Application Support/com.apple.TCC/TCC.db" \
          "/Library/Application Support/com.apple.TCC/TCC.db"; do
  sudo sqlite3 "$db" "
    CREATE TEMP TABLE leave AS SELECT * FROM access WHERE service = 'kTCCServiceMicrophone';
    UPDATE leave SET service = 'kTCCServiceAudioCapture';
    INSERT OR REPLACE INTO access SELECT * FROM leave;" ||
    echo "::warning::cannot change $db"
done
sudo killall tccd || true
