This module provides Queue management for mbuni using PostgreSQL as the storage engine.
To use it, you need only add two lines to the mbuni config 'mbuni' group:

queue-manager-module = "/path_to/libmms_pgsql_queue.so"
queue-module-init-data = "host=dbhost user=db_user password=dbpassword dbname=dbname"

Make sure the database you are trying to connect to has already been created
and the relevant tables created using the supplied file "tables.sql".  

Some notes:
  - Mbuni will open number_of_db_connections connections to the database at startup.
    Ensure your PostgreSQL installation limits are not exceeded. DB connections will
    be shared by all parts of Mbuni, which means that in high traffic situations, 
    you may experience mbuni component slow-down as all db connections might be in use.
  - Vacuum your DB often, since a lot of rows are updated/deleted during operation.

