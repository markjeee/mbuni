-- Table structure for PostgreSQL MMS storage engine
-- (c) 2007 Digital Solutions
-- Licence: See http://mbuni.org/license.shtml
-- Author: P. A. Bagyenda <bagyenda@dsmagic.com>
-- Requires: PostgresQL v8.2 and above

-- Master messages table 
CREATE TABLE mms_messages (
	id bigserial PRIMARY KEY,	
	qdir    varchar(256) NOT NULL,
	qfname 	varchar(256) NOT NULL,
	sender varchar(256) NOT NULL,
	created timestamp with time zone NOT NULL DEFAULT current_timestamp,
	last_try timestamp  with time zone NOT NULL DEFAULT '-infinity',
	send_time timestamp  with time zone  NOT NULL DEFAULT '-infinity',
	expire_date timestamp  with time zone NOT NULL,
	num_attempts int NOT NULL DEFAULT 0,
	
	data bytea NOT NULL DEFAULT '',
	UNIQUE(qdir, qfname)
);

CREATE index mm_idx1 on mms_messages(qdir);  -- because we use it for lookups. 
CREATE index mm_idx2 on mms_messages(send_time);
CREATE index mm_idx3 on mms_messages(qfname);

-- create a view for message lookup 
CREATE VIEW  mms_messages_view AS SELECT 
	 *, 
	EXTRACT(EPOCH FROM created) AS cdate,
	EXTRACT(EPOCH FROM last_try) AS lastt,
	EXTRACT(EPOCH FROM send_time) AS sendt,
	EXTRACT(EPOCH FROM expire_date) AS edate FROM mms_messages;

-- Table for envelope headers.
CREATE TABLE mms_message_headers (
	id bigserial PRIMARY KEY,
	qid bigint REFERENCES mms_messages ON UPDATE CASCADE ON DELETE CASCADE,
	
	item varchar(64) NOT NULL,
	value text NOT NULL	
);

-- When messages are deleted from the queue, they are moved to the achived_XXX tables.
-- archive tables are exact copies of old ones, field for field.
-- DBA should clear these tables as needed
CREATE TABLE archived_mms_messages (LIKE mms_messages INCLUDING DEFAULTS INCLUDING CONSTRAINTS);
CREATE TABLE archived_mms_message_headers (LIKE mms_message_headers INCLUDING DEFAULTS INCLUDING CONSTRAINTS);

ALTER  table archived_mms_messages add unique(id);
ALTER table archived_mms_message_headers add foreign key (qid) 	
	   references archived_mms_messages (id);
