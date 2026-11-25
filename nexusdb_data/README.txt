NexusDB runtime data folder
===========================

This folder is tracked so it exists after cloning the repository on another PC.

Actual database files are generated locally when you run the demo. They are
intentionally ignored by git because they are runtime artifacts and may contain
machine-specific paths, PIDs, and socket locations.

After running the faculty demo, the data file appears here:

  nexusdb_data/faculty/nexusdb.db

The write-ahead log appears beside it:

  nexusdb_data/faculty/nexusdb.wal

Container metadata is stored here:

  nexusdb_data/.nexus/containers/faculty.meta

The database file is a custom binary paged file, not SQLite. Every page is
4096 bytes, and page N is stored at byte offset N * 4096.

Generate the folder contents on another PC from WSL:

  cd ~/NexusDB
  bash scripts/faculty_demo.sh

Or manually with the short wrapper commands:

  cd ~/NexusDB
  nexus create faculty --memory 32M --cpu 50
  nexus start faculty

Then inside the container shell:

  init
  table create notes
  insert notes "Hello faculty"
  scan notes
  exit

View records from the host:

  nexus scan faculty notes
  nexus exec faculty stats
  nexus resources faculty

Sample .db files are committed so the paged file format can be inspected after
cloning. Do not commit .wal, .sock.path, .bash_history, or .meta files. They are
generated on each machine and may not work if copied from another system.
