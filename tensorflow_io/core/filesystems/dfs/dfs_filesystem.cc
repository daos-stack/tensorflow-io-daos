#include "tensorflow_io/core/filesystems/dfs/dfs_utils.h"
namespace tensorflow {
namespace io {
namespace dfs {

// SECTION 1. Implementation for `TF_RandomAccessFile`
// ----------------------------------------------------------------------------
namespace tf_random_access_file {
typedef struct DFSRandomAccessFile {
  std::string dfs_path;
  dfs_t* daos_fs;
  DAOS_FILE daos_file;
  std::vector<ReadBuffer> buffers;
  daos_size_t file_size;
  bool caching;
  size_t buff_size;
  size_t num_of_buffers;
  DFSRandomAccessFile(std::string aDfs_path, dfs_t* file_system, dfs_obj_t* obj, daos_handle_t eq_handle)
      : dfs_path(std::move(aDfs_path)) {
    daos_fs = file_system;
    daos_file.file = obj;
    if(DFS::size_map.count(aDfs_path) == 0) {
    	dfs_get_size(daos_fs, obj, &file_size);
	DFS::size_map[aDfs_path] = file_size;
    }
    else {
	    file_size = DFS::size_map[aDfs_path];
    }
    if(char* env_caching = std::getenv("TF_IO_DAOS_CACHING")) {
      caching = atoi(env_caching) > 0;
    }
    else {
      caching = false;
    }

    if(caching) {
      if (char* env_num_of_buffers = std::getenv("TF_IO_DAOS_NUM_OF_BUFFERS")) {
        num_of_buffers = atoi(env_num_of_buffers);
      } else {
      num_of_buffers = NUM_OF_BUFFERS;
      }

      if (char* env_buff_size = std::getenv("TF_IO_DAOS_BUFFER_SIZE")) {
        buff_size = GetStorageSize(env_buff_size);
      } else {
        buff_size = BUFF_SIZE;
      }
      for (size_t i = 0; i < num_of_buffers; i++) {
        buffers.push_back(ReadBuffer(i, eq_handle, buff_size));
      }
    }
  }

  int64_t ReadNoCache(uint64_t offset, size_t n, char* buffer, TF_Status* status) {
      int rc;
      d_sg_list_t rsgl;
      d_iov_t iov;
      d_iov_set(&iov, (void*)buffer, n);
      rsgl.sg_nr = 1;
      rsgl.sg_iovs = &iov;

      daos_size_t read_size;
      daos_file.offset = offset;

      rc = dfs_read(daos_fs, daos_file.file, &rsgl,
                    daos_file.offset, &read_size, NULL);
      if (rc) {
        TF_SetStatus(status, TF_INTERNAL, "");
        return read_size;
      }

      if (read_size != n) {
        TF_SetStatus(status, TF_OUT_OF_RANGE, "");
        return read_size;
      }

      TF_SetStatus(status, TF_OK, "");
      return read_size;

  }
} DFSRandomAccessFile;

void Cleanup(TF_RandomAccessFile* file) {

  auto dfs_file = static_cast<DFSRandomAccessFile*>(file->plugin_file);
  for (auto& buffer : dfs_file->buffers) {
    buffer.FinalizeEvent();
  }

  dfs_release(dfs_file->daos_file.file);
  dfs_file->daos_fs = nullptr;
  delete dfs_file;
}

int64_t Read(const TF_RandomAccessFile* file, uint64_t offset, size_t n,
             char* ret, TF_Status* status) {
	
  auto dfs_file = static_cast<DFSRandomAccessFile*>(file->plugin_file);
  if (offset >= dfs_file->file_size) {
    TF_SetStatus(status, TF_OUT_OF_RANGE, "");
    return -1;
  }

  if(!dfs_file->caching) {
    return dfs_file->ReadNoCache(offset, n, ret, status);
  }

  size_t ret_offset = 0;
  size_t curr_offset = offset;
  int64_t total_bytes = 0;
  size_t ret_size = offset + n;
  while (curr_offset < ret_size && curr_offset < dfs_file->file_size) {
    size_t read_bytes = 0;
    for (auto& read_buf : dfs_file->buffers) {
      if (read_buf.CacheHit(curr_offset)) {
        read_bytes = read_buf.CopyFromCache(ret, ret_offset, curr_offset, n,
                                            dfs_file->file_size, status);
        curr_offset += read_bytes;
        ret_offset += read_bytes;
        total_bytes += read_bytes;
        n -= read_bytes;
      }
    }

    if(curr_offset >= ret_size || curr_offset >= dfs_file->file_size) break;

    size_t async_offset = curr_offset + dfs_file->buff_size;
    for (size_t i = 1; i < dfs_file->buffers.size(); i++) {    
      if (async_offset > dfs_file->file_size) break;
      dfs_file->buffers[i].ReadAsync(dfs_file->daos_fs,
                                     dfs_file->daos_file.file, async_offset,
                                     dfs_file->file_size);
      async_offset += dfs_file->buff_size;
    }

    dfs_file->buffers[0].ReadSync(dfs_file->daos_fs, dfs_file->daos_file.file,
                                  curr_offset,
                                  dfs_file->file_size);

    read_bytes = dfs_file->buffers[0].CopyFromCache(
        ret, ret_offset, curr_offset, n, dfs_file->file_size, status);

    curr_offset += read_bytes;
    ret_offset += read_bytes;
    total_bytes += read_bytes;
    n -= read_bytes;

    if (curr_offset >= dfs_file->file_size) {
      for (size_t i = 0; i < dfs_file->buffers.size(); i++) {
        dfs_file->buffers[i].WaitEvent();
      }
    }
  }

  return total_bytes;
}

}  // namespace tf_random_access_file

// SECTION 2. Implementation for `TF_WritableFile`
// ----------------------------------------------------------------------------
namespace tf_writable_file {
typedef struct DFSWritableFile {
  std::string dfs_path;
  dfs_t* daos_fs;
  DAOS_FILE daos_file;
  DFSWritableFile(std::string aDfs_path, dfs_t* file_system, dfs_obj_t* obj)
      : dfs_path(std::move(aDfs_path)) {
    daos_fs = file_system;
    daos_file.file = obj;
  }
} DFSWritableFile;

void Cleanup(TF_WritableFile* file) {
  auto dfs_file = static_cast<DFSWritableFile*>(file->plugin_file);
  dfs_release(dfs_file->daos_file.file);
  dfs_file->daos_fs = nullptr;
  delete dfs_file;
}

void Append(const TF_WritableFile* file, const char* buffer, size_t n,
            TF_Status* status) {
  d_sg_list_t wsgl;
  d_iov_t iov;
  int rc;
  auto dfs_file = static_cast<DFSWritableFile*>(file->plugin_file);

  d_iov_set(&iov, (void*)buffer, n);
  wsgl.sg_nr = 1;
  wsgl.sg_iovs = &iov;

  daos_size_t size;
  dfs_get_size(dfs_file->daos_fs, dfs_file->daos_file.file, &size);
  dfs_file->daos_file.offset = size;

  rc = dfs_write(dfs_file->daos_fs, dfs_file->daos_file.file, &wsgl,
                 dfs_file->daos_file.offset, NULL);
  if (rc) {
    TF_SetStatus(status, TF_RESOURCE_EXHAUSTED, "");
  }

  TF_SetStatus(status, TF_OK, "");
}

int64_t Tell(const TF_WritableFile* file, TF_Status* status) {
  auto dfs_file = static_cast<DFSWritableFile*>(file->plugin_file);

  TF_SetStatus(status, TF_OK, "");

  return dfs_file->daos_file.offset;
}

void Close(const TF_WritableFile* file, TF_Status* status) {
  auto dfs_file = static_cast<DFSWritableFile*>(file->plugin_file);
  dfs_release(dfs_file->daos_file.file);
  dfs_file->daos_fs = nullptr;
  dfs_file->daos_file.file = nullptr;
  TF_SetStatus(status, TF_OK, "");
}

}  // namespace tf_writable_file

// SECTION 3. Implementation for `TF_ReadOnlyMemoryRegion`
// ----------------------------------------------------------------------------
namespace tf_read_only_memory_region {
void Cleanup(TF_ReadOnlyMemoryRegion* region) {}

const void* Data(const TF_ReadOnlyMemoryRegion* region) { return nullptr; }

uint64_t Length(const TF_ReadOnlyMemoryRegion* region) { return 0; }

}  // namespace tf_read_only_memory_region

// SECTION 4. Implementation for `TF_Filesystem`, the actual filesystem
// ----------------------------------------------------------------------------
namespace tf_dfs_filesystem {

void Init(TF_Filesystem* filesystem, TF_Status* status) {
  filesystem->plugin_filesystem = new DFS();
  TF_SetStatus(status, TF_OK, "");
}

void Cleanup(TF_Filesystem* filesystem) {
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem);
  daos->dfsCleanup();
  delete daos;
}

void NewFile(const TF_Filesystem* filesystem, const char* path, File_Mode mode,
             int flags, dfs_obj_t** obj, TF_Status* status) {
  int rc;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return;
  }
  std::string pool, cont, file_path;
  rc = daos->Setup(path, pool, cont, file_path, status);

  if (rc) return;
  daos->dfsNewFile(file_path, mode, flags, obj, status);
}

void NewWritableFile(const TF_Filesystem* filesystem, const char* path,
                     TF_WritableFile* file, TF_Status* status) {
  dfs_obj_t* obj = NULL;
  NewFile(filesystem, path, WRITE, S_IWUSR | S_IFREG, &obj, status);
  if (TF_GetCode(status) != TF_OK) return;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return;
  }
  file->plugin_file =
      new tf_writable_file::DFSWritableFile(path, daos->daos_fs, obj);
  TF_SetStatus(status, TF_OK, "");
}

void NewRandomAccessFile(const TF_Filesystem* filesystem, const char* path,
                         TF_RandomAccessFile* file, TF_Status* status) {
  dfs_obj_t* obj = NULL;
  NewFile(filesystem, path, READ, S_IRUSR | S_IFREG, &obj, status);
  if (TF_GetCode(status) != TF_OK) return;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return;
  }
  auto random_access_file =
      new tf_random_access_file::DFSRandomAccessFile(path, daos->daos_fs, obj, daos->mEventQueueHandle);
  if(random_access_file->caching) {
            size_t async_offset = 0;
      for(size_t i = 0; i < random_access_file->num_of_buffers; i++) {
          if (async_offset > random_access_file->file_size) break;
            random_access_file->buffers[i].ReadAsync(
      daos->daos_fs, random_access_file->daos_file.file, async_offset, random_access_file->file_size);
      async_offset += random_access_file->buff_size;
      }
  }
  file->plugin_file = random_access_file;
  TF_SetStatus(status, TF_OK, "");
}

void NewAppendableFile(const TF_Filesystem* filesystem, const char* path,
                       TF_WritableFile* file, TF_Status* status) {
  dfs_obj_t* obj = NULL;
  NewFile(filesystem, path, APPEND, S_IWUSR | S_IFREG, &obj, status);
  if (TF_GetCode(status) != TF_OK) return;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return;
  }
  file->plugin_file =
      new tf_writable_file::DFSWritableFile(path, daos->daos_fs, obj);
  TF_SetStatus(status, TF_OK, "");
}

void PathExists(const TF_Filesystem* filesystem, const char* path,
                TF_Status* status) {
  int rc;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return;
  }
  std::string pool, cont, file;
  rc = daos->Setup(path, pool, cont, file, status);
  if (rc) return;
  dfs_obj_t* obj;
  
  rc = daos->dfsPathExists(file, &obj);
  if (rc) {
    TF_SetStatus(status, TF_NOT_FOUND, "");
  } else {
    TF_SetStatus(status, TF_OK, "");
  }

  dfs_release(obj);
}

void CreateDir(const TF_Filesystem* filesystem, const char* path,
               TF_Status* status) {
  int rc;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return;
  }
  std::string pool, cont, dir_path;
  rc = daos->Setup(path, pool, cont, dir_path, status);
  if (rc) return;

  daos->dfsCreateDir(dir_path, status);
}

static void RecursivelyCreateDir(const TF_Filesystem* filesystem,
                                 const char* path, TF_Status* status) {
  int rc;
  std::string pool, cont, dir_path;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return;
  }
  rc = daos->Setup(path, pool, cont, dir_path, status);
  if (rc) return;

  size_t next_dir = 0;
  std::string dir_string;
  std::string path_string(dir_path);
  do {
    next_dir = path_string.find("/", next_dir);
    dir_string = path_string.substr(0, next_dir);
    if (next_dir != std::string::npos) next_dir++;
    daos->dfsCreateDir(dir_string, status);
    if ((TF_GetCode(status) != TF_OK) &&
        (TF_GetCode(status) != TF_ALREADY_EXISTS))
      return;
    TF_SetStatus(status, TF_OK, "");

  } while (next_dir != std::string::npos);
}

void DeleteFileSystemEntry(const TF_Filesystem* filesystem, const char* path,
                           bool recursive, bool is_dir, TF_Status* status) {
  int rc;
  std::string pool, cont, dir_path;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();

  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return;
  }

  rc = daos->Setup(path, pool, cont, dir_path, status);
  if (rc) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return;
  }
  daos->dfsDeleteObject(dir_path, is_dir, recursive, status);
}

void DeleteSingleDir(const TF_Filesystem* filesystem, const char* path,
                     TF_Status* status) {
  bool recursive = false;
  bool is_dir = true;
  DeleteFileSystemEntry(filesystem, path, recursive, is_dir, status);
}

void RecursivelyDeleteDir(const TF_Filesystem* filesystem, const char* path,
                          uint64_t* undeleted_files, uint64_t* undeleted_dirs,
                          TF_Status* status) {
  bool recursive = true;
  bool is_dir = true;
  DeleteFileSystemEntry(filesystem, path, recursive, is_dir, status);
  if (TF_GetCode(status) == TF_NOT_FOUND ||
      TF_GetCode(status) == TF_FAILED_PRECONDITION) {
    *undeleted_dirs = 1;
    *undeleted_files = 0;
  } else {
    *undeleted_dirs = 0;
    *undeleted_files = 0;
  }
}

bool IsDir(const TF_Filesystem* filesystem, const char* path,
           TF_Status* status) {
  int rc;
  bool is_dir = false;
  std::string pool, cont, file;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return is_dir;
  }
  rc = daos->Setup(path, pool, cont, file, status);
  if (rc) return is_dir;

  if (daos->isRoot(file)) {
    is_dir = true;
    return is_dir;
  }

  dfs_obj_t* obj;
  rc = daos->dfsPathExists(file, &obj, true);
  if (rc) {
    TF_SetStatus(status, TF_NOT_FOUND, "");
  } else {
    is_dir = S_ISDIR(obj->mode);
  }

  if (is_dir) {
    TF_SetStatus(status, TF_OK, "");
  } else {
    TF_SetStatus(status, TF_FAILED_PRECONDITION, "");
  }

  return is_dir;
}

int64_t GetFileSize(const TF_Filesystem* filesystem, const char* path,
                    TF_Status* status) {
  int rc;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return -1;
  }
  std::string pool, cont, file;
  rc = daos->Setup(path, pool, cont, file, status);
  if (rc) return -1;
  if(DFS::size_map.count(path) != 0) {
    return DFS::size_map[path];
  }
  dfs_obj_t* obj;
  rc = daos->dfsPathExists(file, &obj, false);
  if (rc) {
    TF_SetStatus(status, TF_NOT_FOUND, "");
    return -1;
  } 
  else {
    if (S_ISDIR(obj->mode)) {
      TF_SetStatus(status, TF_FAILED_PRECONDITION, "");
      return -1;
    }
    TF_SetStatus(status, TF_OK, "");
    daos_size_t size;
    dfs_get_size(daos->daos_fs, obj, &size);
	  DFS::size_map[path] = size;

    dfs_release(obj);
    return size;
  }
}

void DeleteFile(const TF_Filesystem* filesystem, const char* path,
                TF_Status* status) {
  bool recursive = false;
  bool is_dir = false;
  DeleteFileSystemEntry(filesystem, path, recursive, is_dir, status);
}

void RenameFile(const TF_Filesystem* filesystem, const char* src,
                const char* dst, TF_Status* status) {
  int rc;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return;
  }
  int allow_cont_creation = 1;
  std::string pool_src, cont_src, file_src;
  rc = ParseDFSPath(src, pool_src, cont_src, file_src, daos->pools);
  if (rc) {
    TF_SetStatus(status, TF_FAILED_PRECONDITION, "");
    return;
  }

  std::string pool_dst, cont_dst, file_dst;
  rc = ParseDFSPath(dst, pool_dst, cont_dst, file_dst, daos->pools);
  if (rc) {
    TF_SetStatus(status, TF_FAILED_PRECONDITION, "");
    return;
  }

  if (pool_src != pool_dst || cont_src != cont_dst) {
    TF_SetStatus(status, TF_FAILED_PRECONDITION, "Non-Matching Pool/Container");
    return;
  }

  daos->Connect(pool_src, cont_src, allow_cont_creation, status);
  if (TF_GetCode(status) != TF_OK) {
    TF_SetStatus(status, TF_NOT_FOUND, "");
    return;
  }

  rc = daos->Mount();
  if (rc != 0) {
    TF_SetStatus(status, TF_INTERNAL, "Error Mounting DFS");
    return;
  }

  file_src = "/" + file_src;
  file_dst = "/" + file_dst;

  dfs_obj_t* temp_obj;
  rc = daos->dfsPathExists(file_src, &temp_obj, false);
  if (rc) {
    TF_SetStatus(status, TF_NOT_FOUND, "");
    return;
  } else {
    if (S_ISDIR(temp_obj->mode)) {
      TF_SetStatus(status, TF_FAILED_PRECONDITION, "");
      return;
    }
  }

  dfs_release(temp_obj);

  rc = daos->dfsPathExists(file_dst, &temp_obj, false);
  if (!rc && S_ISDIR(temp_obj->mode)) {
    TF_SetStatus(status, TF_FAILED_PRECONDITION, "");
    return;
  }

  dfs_release(temp_obj);

  dfs_obj_t* parent_src = NULL;
  size_t src_start = file_src.rfind("/") + 1;
  std::string src_name = file_src.substr(src_start);
  rc = daos->dfsFindParent(file_src, &parent_src);
  if (rc) {
    TF_SetStatus(status, TF_NOT_FOUND, "");
    return;
  }

  dfs_obj_t* parent_dst = NULL;
  size_t dst_start = file_dst.rfind("/") + 1;
  std::string dst_name = file_dst.substr(dst_start);
  rc = daos->dfsFindParent(file_dst, &parent_dst);
  if (rc) {
    TF_SetStatus(status, TF_NOT_FOUND, "");
    return;
  }

  if (!S_ISDIR(parent_dst->mode)) {
    TF_SetStatus(status, TF_FAILED_PRECONDITION, "");
    return;
  }

  char* name = (char*)malloc(src_name.size());
  strcpy(name, src_name.c_str());
  char* new_name = (char*)malloc(dst_name.size());
  strcpy(new_name, dst_name.c_str());

  rc = dfs_move(daos->daos_fs, parent_src, name, parent_dst, new_name, NULL);
  free(name);
  free(new_name);
  if (rc) {
    TF_SetStatus(status, TF_INTERNAL, "");
    return;
  }

  TF_SetStatus(status, TF_OK, "");
}

void Stat(const TF_Filesystem* filesystem, const char* path,
          TF_FileStatistics* stats, TF_Status* status) {
  int rc;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return;
  }
  std::string pool, cont, dir_path;
  rc = daos->Setup(path, pool, cont, dir_path, status);
  if (rc) return;

  dfs_obj_t* obj;
  //TODO use actual lookup

  rc = daos->dfsPathExists(dir_path, &obj);
  if (rc) {
    TF_SetStatus(status, TF_NOT_FOUND, "");
    return;
  }

  if (S_ISDIR(obj->mode)) {
    stats->is_directory = true;
    stats->length = 0;
  } else {
    stats->is_directory = false;
    daos_size_t size;
    if(DFS::size_map.count(path) == 0) {
    	dfs_get_size(daos->daos_fs, obj, &size);
	    DFS::size_map[path] = size;
    }
    else {
	    size = DFS:: size_map[path];
    }

    stats->length = size;
  }

  struct stat stbuf;

  dfs_ostat(daos->daos_fs, obj, &stbuf);

  stats->mtime_nsec = static_cast<int64_t>(stbuf.st_mtime) * 1e9;

  dfs_release(obj);
  TF_SetStatus(status, TF_OK, "");
}

int GetChildren(const TF_Filesystem* filesystem, const char* path,
                char*** entries, TF_Status* status) {
  int rc;
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    TF_SetStatus(status, TF_INTERNAL, "Error initializng DAOS API");
    return -1;
  }
  std::string pool, cont, dir_path;
  rc = daos->Setup(path, pool, cont, dir_path, status);
  if (rc) return -1;

  dfs_obj_t* obj;
  rc = daos->dfsPathExists(dir_path, &obj,true);
  if (rc) {
    TF_SetStatus(status, TF_NOT_FOUND, "");
    return -1;
  }

  if (!S_ISDIR(obj->mode)) {
    TF_SetStatus(status, TF_FAILED_PRECONDITION, "");
    return -1;
  }

  std::vector<std::string> children;
  rc = daos->dfsReadDir(obj, children);
  if (rc) {
    TF_SetStatus(status, TF_INTERNAL, "");
    return -1;
  }

  uint32_t nr = children.size();

  CopyEntries(entries, children);

  TF_SetStatus(status, TF_OK, "");
  return nr;
}

static char* TranslateName(const TF_Filesystem* filesystem, const char* uri) {
  return strdup(uri);
}

void FlushCaches(const TF_Filesystem* filesystem) {
  auto daos = static_cast<DFS*>(filesystem->plugin_filesystem)->Load();
  if (!daos) {
    return;
  }
  daos->ClearConnections();
  daos->path_map.clear();
}

}  // namespace tf_dfs_filesystem

void ProvideFilesystemSupportFor(TF_FilesystemPluginOps* ops, const char* uri) {
  TF_SetFilesystemVersionMetadata(ops);
  ops->scheme = strdup(uri);

  ops->random_access_file_ops = static_cast<TF_RandomAccessFileOps*>(
      plugin_memory_allocate(TF_RANDOM_ACCESS_FILE_OPS_SIZE));
  ops->random_access_file_ops->cleanup = tf_random_access_file::Cleanup;
  ops->random_access_file_ops->read = tf_random_access_file::Read;

  ops->writable_file_ops = static_cast<TF_WritableFileOps*>(
      plugin_memory_allocate(TF_WRITABLE_FILE_OPS_SIZE));
  ops->writable_file_ops->cleanup = tf_writable_file::Cleanup;
  ops->writable_file_ops->append = tf_writable_file::Append;
  ops->writable_file_ops->tell = tf_writable_file::Tell;
  ops->writable_file_ops->close = tf_writable_file::Close;

  ops->read_only_memory_region_ops = static_cast<TF_ReadOnlyMemoryRegionOps*>(
      plugin_memory_allocate(TF_READ_ONLY_MEMORY_REGION_OPS_SIZE));
  ops->read_only_memory_region_ops->cleanup =
      tf_read_only_memory_region::Cleanup;
  ops->read_only_memory_region_ops->data = tf_read_only_memory_region::Data;
  ops->read_only_memory_region_ops->length = tf_read_only_memory_region::Length;

  ops->filesystem_ops = static_cast<TF_FilesystemOps*>(
      plugin_memory_allocate(TF_FILESYSTEM_OPS_SIZE));
  ops->filesystem_ops->init = tf_dfs_filesystem::Init;
  ops->filesystem_ops->cleanup = tf_dfs_filesystem::Cleanup;
  ops->filesystem_ops->new_random_access_file =
      tf_dfs_filesystem::NewRandomAccessFile;
  ops->filesystem_ops->new_writable_file = tf_dfs_filesystem::NewWritableFile;
  ops->filesystem_ops->new_appendable_file =
      tf_dfs_filesystem::NewAppendableFile;
  ops->filesystem_ops->path_exists = tf_dfs_filesystem::PathExists;
  ops->filesystem_ops->create_dir = tf_dfs_filesystem::CreateDir;
  ops->filesystem_ops->delete_dir = tf_dfs_filesystem::DeleteSingleDir;
  ops->filesystem_ops->recursively_create_dir =
      tf_dfs_filesystem::RecursivelyCreateDir;
  ops->filesystem_ops->is_directory = tf_dfs_filesystem::IsDir;
  ops->filesystem_ops->delete_recursively =
      tf_dfs_filesystem::RecursivelyDeleteDir;
  ops->filesystem_ops->get_file_size = tf_dfs_filesystem::GetFileSize;
  ops->filesystem_ops->delete_file = tf_dfs_filesystem::DeleteFile;
  ops->filesystem_ops->rename_file = tf_dfs_filesystem::RenameFile;
  ops->filesystem_ops->stat = tf_dfs_filesystem::Stat;
  ops->filesystem_ops->get_children = tf_dfs_filesystem::GetChildren;
  ops->filesystem_ops->translate_name = tf_dfs_filesystem::TranslateName;
  ops->filesystem_ops->flush_caches = tf_dfs_filesystem::FlushCaches;
}

}  // namespace dfs
}  // namespace io
}  // namespace tensorflow
