-- require r/have_federated_db.require
disable_query_log;
show variables like 'have_federated_engine';
enable_query_log;
