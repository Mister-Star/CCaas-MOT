#ifndef _TAAS_STORAGE_H
#define _TAAS_STORAGE_H
//ADDBY TAAS
#include <thread>
#include <vector>
extern std::vector<uint64_t> 
    storage_listener_thread_ids, storage_sender_thread_ids, storage_manager_thread_ids, 
    storage_message_manager_thread_ids, storage_updater_thread_ids, storage_reader_thread_ids;


extern void StorageListenerMain(void);
extern void StorageSenderMain(void);
extern void StorageManagerMain(void);
extern void StorageMessageManagerMain(void);
extern void StorageUpdaterMain(void);
extern void StorageReaderMain(void);


#endif