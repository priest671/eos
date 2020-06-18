#include <eosio/chain/block_log.hpp>
#include <eosio/chain/exceptions.hpp>
#include <fc/bitutil.hpp>
#include <fc/io/cfile.hpp>
#include <fc/io/raw.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/filesystem.hpp>
#include <variant>
#include <algorithm>
#include <regex>

namespace eosio { namespace chain {

   /**
    * History:
    * Version 1: complete block log from genesis
    * Version 2: adds optional partial block log, cannot be used for replay without snapshot
    *            this is in the form of an first_block_num that is written immediately after the version
    * Version 3: improvement on version 2 to not require the genesis state be provided when not starting
    *            from block 1
    * Version 4: changes the block entry from the serialization of signed_block to a tuple of offset to next entry,
    *            compression_status and pruned_block.
    */

   enum versions {
      initial_version = 1,
      block_x_start_version = 2,
      genesis_state_or_chain_id_version = 3,
      pruned_transaction_version = 4
   };

   const uint32_t block_log::min_supported_version = initial_version;
   const uint32_t block_log::max_supported_version = pruned_transaction_version;

   struct block_log_preamble {
      uint32_t version         = 0;
      uint32_t first_block_num = 0;
      std::variant<genesis_state, chain_id_type> chain_context;

      chain_id_type chain_id() const {
         return std::visit(overloaded{[](const chain_id_type& id) { return id; },
                                      [](const genesis_state& state) { return state.compute_chain_id(); }},
                           chain_context);
      }

      constexpr static int nbytes_with_chain_id = // the bytes count when the preamble contains chain_id
          sizeof(version) + sizeof(first_block_num) + sizeof(chain_id_type) + sizeof(block_log::npos);

      void read_from(fc::datastream<const char*>& ds) {
        ds.read((char*)&version, sizeof(version));
         EOS_ASSERT(version > 0, block_log_exception, "Block log was not setup properly");
         EOS_ASSERT(
             block_log::is_supported_version(version), block_log_unsupported_version,
             "Unsupported version of block log. Block log version is ${version} while code supports version(s) "
             "[${min},${max}]",
             ("version", version)("min", block_log::min_supported_version)("max", block_log::max_supported_version));

         first_block_num = 1;
         if (version != initial_version) {
            ds.read((char*)&first_block_num, sizeof(first_block_num));
         }

         if (block_log::contains_genesis_state(version, first_block_num)) {
            chain_context.emplace<genesis_state>();
            fc::raw::unpack(ds, std::get<genesis_state>(chain_context));
         } else if (block_log::contains_chain_id(version, first_block_num)) {
            chain_context = chain_id_type{};
            ds >> std::get<chain_id_type>(chain_context);
         } else {
            EOS_THROW(block_log_exception,
                      "Block log is not supported. version: ${ver} and first_block_num: ${fbn} does not contain "
                      "a genesis_state nor a chain_id.",
                      ("ver", version)("fbn", first_block_num));
         }

         if (version != initial_version) {
            auto                                    expected_totem = block_log::npos;
            std::decay_t<decltype(block_log::npos)> actual_totem;
            ds.read((char*)&actual_totem, sizeof(actual_totem));

            EOS_ASSERT(
                actual_totem == expected_totem, block_log_exception,
                "Expected separator between block log header and blocks was not found( expected: ${e}, actual: ${a} )",
                ("e", fc::to_hex((char*)&expected_totem, sizeof(expected_totem)))(
                    "a", fc::to_hex((char*)&actual_totem, sizeof(actual_totem))));
         }
      }

      template <typename Stream>
      void write_to(Stream& ds) const {
         ds.write(reinterpret_cast<const char*>(&version), sizeof(version));
         if (version != initial_version) {
            ds.write(reinterpret_cast<const char*>(&first_block_num), sizeof(first_block_num));

            std::visit(overloaded{[&ds](const chain_id_type& id) { ds << id; },
                                 [&ds](const genesis_state& state) {
                                    auto data = fc::raw::pack(state);
                                    ds.write(data.data(), data.size());
                                 }},
                     chain_context);

            auto totem = block_log::npos;
            ds.write(reinterpret_cast<const char*>(&totem), sizeof(totem));
         }
         else {
            const auto& state = std::get<genesis_state>(chain_context);
            auto data = fc::raw::pack(state);
            ds.write(data.data(), data.size());
         }
      }
   };

   struct log_entry_v4 {
         // In version 4 of the irreversible blocks log format, these log entries consists of the following in order:
         //    1. An uint32_t size for number of bytes from the start of this log entry to the start of the next log entry.
         //    2. An uint8_t indicating the compression status for the serialization of the pruned_block following this.
         //    3. The serialization of a signed_block representation of the block for the entry including padding.

         struct metadata_type {
            packed_transaction::cf_compression_type compression = packed_transaction::cf_compression_type::none;
            uint32_t size = 0; // the size of the log entry
         };

         metadata_type meta;
         signed_block  block;
   };

   namespace {

      template <typename T>
      T read_buffer(const char* buf) {
         T result;
         memcpy(&result, buf, sizeof(T));
         return result;
      }

      /// calculate the offset from the start of serialized block entry to block start
      constexpr int offset_to_block_start(uint32_t version) { 
         return version >= pruned_transaction_version ? sizeof(uint32_t) + 1 : 0;
      }

      template <typename Stream>
      log_entry_v4::metadata_type unpack(Stream& ds, signed_block& block){
         log_entry_v4::metadata_type meta;
         const auto                  start_pos = ds.tellp();
         fc::raw::unpack(ds, meta.size);
         uint8_t compression;
         fc::raw::unpack(ds, compression);
         EOS_ASSERT(compression < static_cast<uint8_t>(packed_transaction::cf_compression_type::COMPRESSION_TYPE_COUNT), block_log_exception, 
                  "Unknown compression_type");
         meta.compression = static_cast<packed_transaction::cf_compression_type>(compression);
         EOS_ASSERT(meta.compression == packed_transaction::cf_compression_type::none, block_log_exception,
                  "Only support compression_type none");         
         block.unpack(ds, meta.compression);
         const uint64_t current_stream_offset = ds.tellp() - start_pos;
         // For a block which contains CFD (context free data) and the CFD is pruned afterwards, the entry.size may
         // be the size before the CFD has been pruned while the actual serialized block does not have the CFD anymore.
         // In this case, the serialized block has fewer bytes than what's indicated by entry.size. We need to
         // skip over the extra bytes to allow ds to position to the last 8 bytes of the entry.  
         const int64_t bytes_to_skip = static_cast<int64_t>(meta.size) - sizeof(uint64_t) - current_stream_offset;
         EOS_ASSERT(bytes_to_skip >= 0, block_log_exception,
                    "Invalid block log entry size");
         ds.skip(bytes_to_skip);
         return meta;
      }

      template <typename Stream>
      void unpack(Stream& ds, log_entry_v4& entry){
         entry.meta = unpack(ds, entry.block);
      }

      std::vector<char> pack(const signed_block& block, packed_transaction::cf_compression_type compression) {
         const std::size_t padded_size = block.maximum_pruned_pack_size(compression);
         static_assert( block_log::max_supported_version == pruned_transaction_version,
                     "Code was written to support format of version 4, need to update this code for latest format." );
         std::vector<char>     buffer(padded_size + offset_to_block_start(block_log::max_supported_version));
         fc::datastream<char*> stream(buffer.data(), buffer.size());

         const uint32_t size = buffer.size() + sizeof(uint64_t);
         stream.write((char*)&size, sizeof(size));
         fc::raw::pack(stream, static_cast<uint8_t>(compression));
         block.pack(stream, compression);
         return buffer;
      }

      using log_entry = std::variant<log_entry_v4, signed_block_v0>;

      template <typename Stream>
      void unpack(Stream& ds, log_entry& entry) {
         std::visit(
             overloaded{[&ds](signed_block_v0& v) { fc::raw::unpack(ds, v); }, 
                        [&ds](log_entry_v4& v) { unpack(ds, v); }},
             entry);
      }

   void create_mapped_file(boost::iostreams::mapped_file_sink& sink, const std::string& path, uint64_t size) {
      using namespace boost::iostreams;
      mapped_file_params params(path);
      params.flags         = mapped_file::readwrite;
      params.new_file_size = size;
      params.length        = size;
      params.offset        = 0;
      sink.open(params);
   }

   class index_writer {
    public:
      index_writer(const fc::path& block_index_name, uint32_t blocks_expected)
          : current_offset(blocks_expected * sizeof(uint64_t)) {
         create_mapped_file(index, block_index_name.generic_string(), current_offset);
      }
      void write(uint64_t pos) {
         current_offset -= sizeof(pos);
         memcpy(index.data() + current_offset, &pos, sizeof(pos));
      }

      void close() { index.close(); }

    private:
      std::ptrdiff_t                     current_offset = 0;
      boost::iostreams::mapped_file_sink index;
   };

   struct bad_block_excpetion {
      std::exception_ptr inner;
   };

   template <typename Stream>
   std::unique_ptr<signed_block> read_block(Stream&& ds, uint32_t version, uint32_t expect_block_num = 0) {
      std::unique_ptr<signed_block> block;
      if (version >= pruned_transaction_version) {
         block = std::make_unique<signed_block>();
         unpack(ds, *block);
      } else {
         signed_block_v0 block_v0;
         fc::raw::unpack(ds, block_v0);
         block = std::make_unique<signed_block>(std::move(block_v0), true);
      }

      if (expect_block_num != 0) {
         EOS_ASSERT(!!block && block->block_num() == expect_block_num, block_log_exception,
                     "Wrong block was read from block log.");
      }

      return block;
   }

   template <typename Stream>
   block_id_type read_block_id(Stream&& ds, uint32_t version, uint32_t expect_block_num) {
      if (version >= pruned_transaction_version) {
         uint32_t size;
         uint8_t  compression;
         fc::raw::unpack(ds, size);
         fc::raw::unpack(ds, compression);
         EOS_ASSERT(compression == static_cast<uint8_t>(packed_transaction::cf_compression_type::none),
                     block_log_exception, "Only \"none\" compression type is supported.");
      }
      block_header bh;
      fc::raw::unpack(ds, bh);

      EOS_ASSERT(bh.block_num() == expect_block_num, block_log_exception,
                  "Wrong block header was read from block log.",
                  ("returned", bh.block_num())("expected", expect_block_num));

      return bh.calculate_id();
   }

   /// Provide the read only view of the blocks.log file
   class block_log_data {
      boost::iostreams::mapped_file_source file;
      block_log_preamble                   preamble;
      uint64_t                             first_block_pos = block_log::npos;
   public:
      block_log_data() = default;
      block_log_data(const fc::path& path) { open(path); }

      const block_log_preamble& get_preamble() const { return preamble; }

      fc::datastream<const char*> open(const fc::path& path) {
         if (file.is_open())
            file.close();
         file.open(path.generic_string());
         fc::datastream<const char*> ds(file.data(), file.size());
         preamble.read_from(ds);
         first_block_pos = ds.tellp();
         return ds;
      }
      
      bool is_open() const { return file.is_open(); }

      const char*   data() const { return file.data(); }
      uint64_t      size() const { return file.size(); }
      uint32_t      version() const { return preamble.version; }
      uint32_t      first_block_num() const { return preamble.first_block_num; }
      uint32_t      last_block_num() const { return block_num_at(last_block_position()); }
      uint64_t      first_block_position() const { return first_block_pos; }
      uint64_t      last_block_position() const { return read_buffer<uint64_t>(data() + size() - sizeof(uint64_t)); }
      chain_id_type chain_id() const { return preamble.chain_id(); }

      fc::optional<genesis_state> get_genesis_state() const {
         return std::visit(overloaded{[](const chain_id_type&) { return fc::optional<genesis_state>{}; },
                                      [](const genesis_state& state) { return fc::optional<genesis_state>{state}; }},
                           preamble.chain_context);
      }

      uint32_t block_num_at(uint64_t position) const {
         // to derive blknum_offset==14 see block_header.hpp and note on disk struct is packed
         //   block_timestamp_type timestamp;                  //bytes 0:3
         //   account_name         producer;                   //bytes 4:11
         //   uint16_t             confirmed;                  //bytes 12:13
         //   block_id_type        previous;                   //bytes 14:45, low 4 bytes is big endian block number of
         //   previous block

         EOS_ASSERT(position <= size(), block_log_exception, "Invalid block position ${position}", ("position", position));

         int blknum_offset = 14;
         blknum_offset += offset_to_block_start(version());
         uint32_t prev_block_num = read_buffer<uint32_t>(data() + position + blknum_offset);
         return fc::endian_reverse_u32(prev_block_num) + 1;
      }

      uint32_t num_blocks() const {
         if (first_block_pos == file.size())
            return 0;
         return last_block_num() - first_block_num() + 1;
      }

      fc::datastream<const char*> datastream_at(uint64_t pos) {
         return fc::datastream<const char*>(data() + pos, file.size() - pos);
      }

      /**
       *  Validate a block log entry WITHOUT deserializing the entire block data.
       **/
      void light_validate_block_entry_at(uint64_t pos, uint32_t expected_block_num) const {
         const uint32_t actual_block_num = block_num_at(pos);

         EOS_ASSERT(actual_block_num == expected_block_num, block_log_exception,
                    "At position ${pos} expected to find block number ${exp_bnum} but found ${act_bnum}",
                    ("pos", pos)("exp_bnum", expected_block_num)("act_bnum", actual_block_num));

         if (version() >= pruned_transaction_version) {
            uint32_t entry_size     = read_buffer<uint32_t>(data()+pos);
            uint64_t entry_position = read_buffer<uint64_t>(data() + pos + entry_size - sizeof(uint64_t));
            EOS_ASSERT(pos == entry_position, block_log_exception, 
               "The last 8 bytes in the block entry of block number ${n} does not contain its own position", ("n", actual_block_num));
         }
      }
   
      /**
       *  Validate a block log entry by deserializing the entire block data. 
       * 
       *  @returns The tuple of block number and block id in the entry
       **/
      static std::tuple<uint32_t, block_id_type> 
      full_validate_block_entry(fc::datastream<const char*>& ds, uint32_t previous_block_num, const block_id_type& previous_block_id, log_entry& entry) {
         uint64_t pos = ds.tellp();

         try {
            unpack(ds, entry);
         } catch (...) {
            throw bad_block_excpetion{std::current_exception()};
         }

         const block_header& header =
             std::visit(overloaded{[](const signed_block_v0& v) -> const block_header& { return v; },
                                   [](const log_entry_v4& v) -> const block_header& { return v.block; }},
                        entry);

         auto                id        = header.calculate_id();
         auto                block_num = block_header::num_from_id(id);

         if (block_num != previous_block_num + 1) {
            elog( "Block ${num} (${id}) skips blocks. Previous block in block log is block ${prev_num} (${previous})",
                  ("num", block_num)("id", id)
                  ("prev_num", previous_block_num)("previous", previous_block_id) );
         }

         if (previous_block_id != block_id_type() && previous_block_id != header.previous) {
            elog("Block ${num} (${id}) does not link back to previous block. "
               "Expected previous: ${expected}. Actual previous: ${actual}.",
               ("num", block_num)("id", id)("expected", previous_block_id)("actual", header.previous));
         }

         uint64_t tmp_pos = std::numeric_limits<uint64_t>::max();
         if (ds.remaining() >= sizeof(tmp_pos)) {
            ds.read(reinterpret_cast<char*>(&tmp_pos), sizeof(tmp_pos));
         }

         EOS_ASSERT(pos == tmp_pos, block_log_exception, "the block position for block ${num} at the end of a block entry is incorrect", ("num", block_num));
         return std::make_tuple(block_num, id);
      }
   };

   /// Provide the read only view of the blocks.index file
   class block_log_index {
      boost::iostreams::mapped_file_source file;

    public:
      block_log_index() = default;
      block_log_index(const fc::path& path) { open(path); }

      void open(const fc::path& path) { 
         if (file.is_open())
            file.close();
         file.open(path.generic_string());
         EOS_ASSERT(file.size() % sizeof(uint64_t) == 0, block_log_exception, "The size of ${file} is not the multiple of sizeof(uint64_t)",
            ("file", path.generic_string()));
      }

      bool is_open() const { return file.is_open(); }

      using iterator = const uint64_t*;
      iterator begin() const { return reinterpret_cast<iterator>(file.data()); }
      iterator end() const { return reinterpret_cast<iterator>(file.data() + file.size()); }

      /// @pre file.size() > 0
      uint64_t back() const { return *(this->end() - 1); }
      int      num_blocks() const { return file.size() / sizeof(uint64_t); }
      uint64_t nth_block_position(uint32_t n) const { return *(begin() + n); }
   };

   /// Provide the read only view for both blocks.log and blocks.index files
   struct block_log_archive {
      fc::path        block_file_name, index_file_name; // full pathname for blocks.log and blocks.index
      block_log_data  log_data;
      block_log_index log_index;

      block_log_archive(fc::path block_dir) {
         block_file_name = block_dir / "blocks.log";
         index_file_name = block_dir / "blocks.index";

         log_data.open(block_file_name);
         log_index.open(index_file_name);

         uint32_t log_num_blocks   = log_data.num_blocks();
         uint32_t index_num_blocks = log_index.num_blocks();

         EOS_ASSERT(
             log_num_blocks == index_num_blocks, block_log_exception,
             "${block_file_name} says it has ${log_num_blocks} blocks which disagrees with ${index_num_blocks} indicated by ${index_file_name}",
             ("block_file_name", block_file_name)("log_num_blocks", log_num_blocks)("index_num_blocks", index_num_blocks)("index_file_name", index_file_name));
      }
   };

   /// Used to traverse the block position (i.e. the last 8 bytes in each block log entry) of the blocks.log file
   template <typename T>
   struct reverse_block_position_iterator {
      const T& data;
      uint64_t begin_position;
      uint64_t current_position;
      reverse_block_position_iterator(const T& data, uint64_t first_block_pos)
          : data(data)
          , begin_position(first_block_pos - sizeof(uint64_t))
          , current_position(data.size() - sizeof(uint64_t)) {}

      auto addr() const { return data.data() + current_position; }

      uint64_t get_value() {
         if (current_position <= begin_position)
            return block_log::npos;
         return read_buffer<uint64_t>(addr());
      }

      void set_value(uint64_t pos) { memcpy(addr(), &pos, sizeof(pos)); }

      reverse_block_position_iterator& operator++() {
         EOS_ASSERT(current_position > begin_position && current_position < data.size(), block_log_exception,
                    "Block log file formatting is incorrect, it contains a block position value: ${pos}, which is not "
                    "in the range of (${begin_pos},${last_pos})",
                    ("pos", current_position)("begin_pos", begin_position)("last_pos", data.size()));

         current_position = read_buffer<uint64_t>(addr()) - sizeof(uint64_t);
         return *this;
      }
   };

   template <typename BlockLogData>
   reverse_block_position_iterator<BlockLogData> make_reverse_block_position_iterator(const BlockLogData& t) {
      return reverse_block_position_iterator<BlockLogData>(t, t.first_block_position());
   }

   template <typename BlockLogData>
   reverse_block_position_iterator<BlockLogData> make_reverse_block_position_iterator(const BlockLogData& t,
                                                                                     uint64_t first_block_position) {
      return reverse_block_position_iterator<BlockLogData>(t, first_block_position);
   }

   template <typename Lambda>
   void for_each_file_in_dir_matches(const fc::path& dir, const char* pattern, Lambda&& lambda) {
      const std::regex my_filter(pattern);
      std::smatch      what;
      boost::filesystem::directory_iterator end_itr; // Default ctor yields past-the-end
      for (boost::filesystem::directory_iterator p(dir); p != end_itr; ++p) {
         // Skip if not a file
         if (!boost::filesystem::is_regular_file(p->status()))
            continue;
         // skip if it's not match blocks-*-*.log
         if (!std::regex_match(p->path().filename().string(), what, my_filter))
            continue;
         lambda(p->path());
      }
   } 
   } // namespace

   struct block_log_catalog {
      using block_num_t = uint32_t;

      struct mapped_type {
         block_num_t             last_block_num;
         boost::filesystem::path filename_base;
      };
      using collection_t              = boost::container::flat_map<block_num_t, mapped_type>;
      using size_type                 = collection_t::size_type;
      static constexpr size_type npos = std::numeric_limits<size_type>::max();

      boost::filesystem::path    archive_dir;
      size_type                  max_retained_files = 10;
      collection_t               collection;
      size_type                  active_index = npos;
      block_log_data             log_data;
      block_log_index            log_index;
      chain_id_type              chain_id;

      bool empty() const { return collection.empty();  }

      void open(fc::path block_dir) {
         for_each_file_in_dir_matches(block_dir, R"(blocks-\d+-\d+\.log)", [this](boost::filesystem::path path) {
            auto log_path               = path;
            auto index_path             = path.replace_extension("index");
            auto path_without_extension = log_path.parent_path() / log_path.stem().string();

            block_log_data log(log_path);

            if (chain_id.empty()) {
               chain_id = log.chain_id();
            } else {
               EOS_ASSERT(chain_id == log.chain_id(), block_log_exception, "block log file ${path} has a different chain id",
                           ("path", log_path.generic_string()));
            }

            // check if index file matches the log file
            if (!index_matches_data(index_path, log))
               block_log::construct_index(log_path, index_path);

            auto existing_itr = collection.find(log.first_block_num());
            if (existing_itr != collection.end()) {
               if (log.last_block_num() <= existing_itr->second.last_block_num) {
                  wlog("${log_path} contains the overlapping range with ${existing_path}.log, droping ${log_path} "
                       "from catelog",
                       ("log_path", log_path.string())("existing_path", existing_itr->second.filename_base.string()));
                  return;
               }
               else {
                  wlog("${log_path} contains the overlapping range with ${existing_path}.log, droping ${existing_path}.log "
                       "from catelog",
                       ("log_path", log_path.string())("existing_path", existing_itr->second.filename_base.string()));
               }
            }

            collection.insert_or_assign(log.first_block_num(), mapped_type{ log.last_block_num(), path_without_extension });
         });
      }  

      bool index_matches_data(const boost::filesystem::path& index_path, const block_log_data& log) const {
         if (boost::filesystem::exists(index_path) &&
             boost::filesystem::file_size(index_path) / sizeof(uint64_t) != log.num_blocks()) {
            // make sure the last 8 bytes of index and log matches

            fc::cfile index_file;
            index_file.set_file_path(index_path);
            index_file.open("r");
            index_file.seek_end(-sizeof(uint64_t));
            uint64_t pos;
            index_file.read(reinterpret_cast<char*>(&pos), sizeof(pos));
            return pos == log.last_block_position();
         }
         return false;
      }

      bool set_active_item(uint32_t block_num) {
         try {
            if (active_index != npos) {
               auto active_item = collection.nth(active_index);
               if (active_item->first <= block_num && block_num <= active_item->second.last_block_num) {
                  if (!log_index.is_open()) {
                     log_index.open(active_item->second.filename_base.replace_extension("index"));
                  }
                  return true;
               }
            }
            if (collection.empty() || block_num < collection.begin()->first )
               return false;

            auto it = --collection.upper_bound(block_num);

            if (block_num <= it->second.last_block_num ) {
               auto name = it->second.filename_base;
               log_data.open(name.replace_extension("log"));
               log_index.open(name.replace_extension("index"));
               this->active_index = collection.index_of(it);
               return true;
            }
            return false;
         } catch (...) {
            this->active_index = npos;
            return false;
         }
      }

      std::pair<fc::datastream<const char*>, uint32_t> datastream_for_block(uint32_t block_num) {
         auto pos = log_index.nth_block_position(block_num - log_data.first_block_num());
         return std::make_pair(log_data.datastream_at(pos), log_data.get_preamble().version);
      }

      /// Add a new entry into the catalog.
      ///
      /// Notice that \c start_block_num must be monotonically increasing between the invocations of this function
      /// so that the new entry would be inserted at the end of the flat_map; otherwise, \c active_index would be invalidated
      /// and the mapping between the log data their block range would be wrong. This function is only used during 
      /// the splitting of block log. Using this function for other purpose should make sure if the monotonically 
      /// increasing block num guarantee can be met.
      void add(uint32_t start_block_num, uint32_t end_block_num, fc::path filename_base) {
         if (this->collection.size() >= max_retained_files) {
            auto items_to_erase = max_retained_files > 0 ? this->collection.size() - max_retained_files + 1 : this->collection.size();
            for (auto it = this->collection.begin(); it < this->collection.begin() + items_to_erase; ++it) {
               auto name = it->second.filename_base;
               if (archive_dir.empty()) {
                  // delete the old files when no backup dir is specified
                  boost::filesystem::remove(name.replace_extension("log"));
                  boost::filesystem::remove(name.replace_extension("index"));
               } else {
                  // move the the backup dir
                  auto new_name = archive_dir / boost::filesystem::path(name).filename();
                  boost::filesystem::rename(name.replace_extension("log"), new_name.replace_extension("log"));
                  boost::filesystem::rename(name.replace_extension("index"), new_name.replace_extension("index"));
               }
            }
            this->collection.erase(this->collection.begin(), this->collection.begin() + items_to_erase);
            this->active_index = this->active_index == npos  || this->active_index < items_to_erase ? npos : this->active_index - items_to_erase;
         }
         if (max_retained_files > 0)
            this->collection.emplace(start_block_num, mapped_type{end_block_num, filename_base});
      }
   };

   namespace detail {

      /**
       * The implementation detail for the read/write access to the block log/index
       *
       * @note All the non-static member functions require to fullfill the class invariant after execution unless
       *exceptions are thrown.
       * @invariant block_file.is_open() && index_file.is_open()
       **/
      class block_log_impl {
       public:
         signed_block_ptr          head;
         block_log_catalog         catalog;
         fc::datastream<fc::cfile> block_file;
         fc::datastream<fc::cfile> index_file;
         bool                      genesis_written_to_block_log = false;
         block_log_preamble        preamble;
         size_t                    stride = std::numeric_limits<size_t>::max();
         static uint32_t           default_version;

         block_log_impl(const fc::path& data_dir, fc::path archive_dir, uint64_t stride, uint16_t max_retained_files);

         static void ensure_file_exists(fc::cfile& f) {
            if (fc::exists(f.get_file_path()))
               return;
            f.open(fc::cfile::create_or_update_rw_mode);
            f.close();
         }

         uint64_t get_block_pos(uint32_t block_num);

         void reset(uint32_t first_block_num, std::variant<genesis_state, chain_id_type>&& chain_context);

         void flush();

         uint64_t append(const signed_block_ptr& b, packed_transaction::cf_compression_type segment_compression);

         uint64_t write_log_entry(const signed_block& b, packed_transaction::cf_compression_type segment_compression);

         void split_log();
         bool recover_from_incomplete_block_head(block_log_data& log_data, block_log_index& index);

         block_id_type                 read_block_id_by_num(uint32_t block_num);
         std::unique_ptr<signed_block> read_block_by_num(uint32_t block_num);
         void                          read_head();
      };
      uint32_t block_log_impl::default_version = block_log::max_supported_version;
   } // namespace detail

   block_log::block_log(const fc::path& data_dir, fc::path archive_dir, uint64_t stride,
                        uint16_t max_retained_files)
       : my(new detail::block_log_impl(data_dir, archive_dir, stride, max_retained_files)) {}

   block_log::~block_log() {}

   bool detail::block_log_impl::recover_from_incomplete_block_head(block_log_data& log_data, block_log_index& index) {
      if (preamble.version >= pruned_transaction_version) {
         // check if the last block position
         if (log_data.size() > index.back() + sizeof(uint32_t)) {
            const uint32_t entry_size              = read_buffer<uint32_t>(log_data.data() + index.back());
            const uint64_t trimmed_block_file_size = index.back() + entry_size;
            const uint32_t expected_block_num      = log_data.first_block_num() + index.num_blocks() - 1;
            if (log_data.size() > trimmed_block_file_size) {
               try {
                  log_data.light_validate_block_entry_at(index.back(), expected_block_num);
                  ilog("The last block from blocks.log is incomplete, trim it.");
                  boost::filesystem::resize_file(block_file.get_file_path(), trimmed_block_file_size);
                  return true;
               }
               catch(...) {
                  return false;
               }
            }
         }
      }
      return false;
   }

   void block_log::set_version(uint32_t ver) { detail::block_log_impl::default_version = ver; }
   uint32_t block_log::version() const { return my->preamble.version; }

   detail::block_log_impl::block_log_impl(const fc::path& data_dir, fc::path archive_dir, uint64_t stride,
                                          uint16_t max_retained_files) {

      if (!fc::is_directory(data_dir))
         fc::create_directories(data_dir);
      else
         catalog.open(data_dir);

      if (! archive_dir.empty()) {
         if (archive_dir.is_relative())
            archive_dir = data_dir / archive_dir;

         if (!fc::is_directory(archive_dir))
            fc::create_directories(archive_dir);
      }
      

      catalog.archive_dir        = archive_dir;
      catalog.max_retained_files = max_retained_files;
      this->stride               = stride;

      block_file.set_file_path(data_dir / "blocks.log");
      index_file.set_file_path(data_dir / "blocks.index");
      /* On startup of the block log, there are several states the log file and the index file can be
       * in relation to each other.
       *
       *                          Block Log
       *                     Exists       Is New
       *                 +------------+------------+
       *          Exists |    Check   |   Delete   |
       *   Index         |    Head    |    Index   |
       *    File         +------------+------------+
       *          Is New |   Replay   |     Do     |
       *                 |    Log     |   Nothing  |
       *                 +------------+------------+
       *
       * Checking the heads of the files has several conditions as well.
       *  - If they are the same, do nothing.
       *  - If the index file head is not in the log file, delete the index and replay.
       *  - If the index file head is in the log, but not up to date, replay from index head.
       */
      ensure_file_exists(block_file);
      ensure_file_exists(index_file);
      const auto log_size   = fc::file_size(block_file.get_file_path());
      const auto index_size = fc::file_size(index_file.get_file_path());

      if (log_size) {
         ilog("Log is nonempty");
         block_log_data log_data(block_file.get_file_path());
         preamble = log_data.get_preamble();

         EOS_ASSERT(catalog.chain_id.empty() || catalog.chain_id == preamble.chain_id(), block_log_exception,
                    "block log file ${path} has a different chain id", ("path", block_file.get_file_path()));

         genesis_written_to_block_log = true; // Assume it was constructed properly.

         if (index_size) {
            ilog("Index is nonempty");
            block_log_index index(index_file.get_file_path());
            
            if (log_data.last_block_position() != index.back()) {              
               if (!recover_from_incomplete_block_head(log_data, index)) {
                  ilog("The last block positions from blocks.log and blocks.index are different, Reconstructing index...");
                  block_log::construct_index(block_file.get_file_path(), index_file.get_file_path());
               }  
            }
         } else {
            ilog("Index is empty. Reconstructing index...");
            block_log::construct_index(block_file.get_file_path(), index_file.get_file_path());
         }
      } else if (index_size) {
         ilog("Log file is empty while the index file is nonempty, discard the index file");
         boost::filesystem::resize_file(index_file.get_file_path(), 0);
      }

      block_file.open(fc::cfile::update_rw_mode);
      index_file.open(fc::cfile::update_rw_mode);
      if (log_size)
         read_head();
   }

   uint64_t detail::block_log_impl::write_log_entry(const signed_block& b, packed_transaction::cf_compression_type segment_compression) {
      uint64_t pos = block_file.tellp();
      std::vector<char> buffer;
     
      if (preamble.version >= pruned_transaction_version)  {
         buffer = pack(b, segment_compression);
      } else {
         auto block_ptr = b.to_signed_block_v0();
         EOS_ASSERT(block_ptr, block_log_append_fail, "Unable to convert block to legacy format");
         EOS_ASSERT(segment_compression == packed_transaction::cf_compression_type::none, block_log_append_fail,
            "the compression must be \"none\" for legacy format");
         buffer = fc::raw::pack(*block_ptr);
      }
      block_file.write(buffer.data(), buffer.size());
      block_file.write((char*)&pos, sizeof(pos));
      index_file.write((char*)&pos, sizeof(pos));
      flush();
      return pos;
   }

   uint64_t block_log::append(const signed_block_ptr& b, packed_transaction::cf_compression_type segment_compression) {
      return my->append(b, segment_compression);
   }

   uint64_t detail::block_log_impl::append(const  signed_block_ptr& b, packed_transaction::cf_compression_type segment_compression) {
      try {
         EOS_ASSERT( genesis_written_to_block_log, block_log_append_fail, "Cannot append to block log until the genesis is first written" );

         block_file.seek_end(0);
         index_file.seek_end(0);
         EOS_ASSERT(index_file.tellp() == sizeof(uint64_t) * (b->block_num() - preamble.first_block_num),
                   block_log_append_fail,
                   "Append to index file occuring at wrong position.",
                   ("position", (uint64_t) index_file.tellp())
                   ("expected", (b->block_num() - preamble.first_block_num) * sizeof(uint64_t)));

         auto pos = write_log_entry(*b, segment_compression);
         head     = b;
         if (b->block_num() % stride == 0) {
            split_log();
         }
         return pos;
      }
      FC_LOG_AND_RETHROW()
   }

   void detail::block_log_impl::split_log() { 
      block_file.close();
      index_file.close();

      auto      data_dir = block_file.get_file_path().parent_path();
      const int bufsize  = 64;
      char      filename[bufsize];
      int n = snprintf(filename, bufsize, "blocks-%d-%d", preamble.first_block_num, this->head->block_num());
      catalog.add(preamble.first_block_num, this->head->block_num(), data_dir / filename);
      strncpy(filename + n, ".log", bufsize-n);
      boost::filesystem::rename(block_file.get_file_path(), data_dir / filename);
      strncpy(filename + n, ".index", bufsize-n);
      boost::filesystem::rename(index_file.get_file_path(), data_dir / filename);
      
      block_file.open(fc::cfile::truncate_rw_mode);
      index_file.open(fc::cfile::truncate_rw_mode);
      preamble.version         = block_log::max_supported_version;
      preamble.chain_context   = preamble.chain_id();
      preamble.first_block_num = this->head->block_num() + 1;
      preamble.write_to(block_file);
      flush();
   }

   void detail::block_log_impl::flush() {
      block_file.flush();
      index_file.flush();
   }

   void detail::block_log_impl::reset(uint32_t first_bnum, std::variant<genesis_state, chain_id_type>&& chain_context) {

      block_file.open(fc::cfile::truncate_rw_mode);
      index_file.open(fc::cfile::truncate_rw_mode);

      preamble.version         = block_log_impl::default_version; 
      preamble.first_block_num = first_bnum;
      preamble.chain_context   = std::move(chain_context);
      preamble.write_to(block_file);

      flush();
      genesis_written_to_block_log = true;
      static_assert( block_log::max_supported_version > 0, "a version number of zero is not supported" );
   }

   void block_log::reset( const genesis_state& gs, const signed_block_ptr& first_block, packed_transaction::cf_compression_type segment_compression ) {
      my->reset(1, gs);
      append(first_block, segment_compression);
   }

   void block_log::reset(const chain_id_type& chain_id, uint32_t first_block_num) {
      EOS_ASSERT(first_block_num > 1, block_log_exception,
                 "Block log version ${ver} needs to be created with a genesis state if starting from block number 1.");

      EOS_ASSERT(my->catalog.chain_id.empty() || chain_id == my->catalog.chain_id, block_log_exception,
                 "Trying to reset to the chain to a different chain id");

      my->reset(first_block_num, chain_id);
      my->head.reset();
   }

   std::unique_ptr<signed_block> detail::block_log_impl::read_block_by_num(uint32_t block_num) {
      uint64_t pos = get_block_pos(block_num);
      if (pos != block_log::npos) {
         block_file.seek(pos);
         return read_block(block_file, preamble.version, block_num);
      } else if (catalog.set_active_item(block_num)) {
         auto [ds, version] = catalog.datastream_for_block(block_num);
         return read_block(ds, version, block_num);
      }
      return {};
   }

   block_id_type detail::block_log_impl::read_block_id_by_num(uint32_t block_num) {
      uint64_t pos = get_block_pos(block_num);
      if (pos != block_log::npos) {
         block_file.seek(pos);
         return read_block_id(block_file, preamble.version, block_num);
      } else if (catalog.set_active_item(block_num)) {
         auto [ds, version] = catalog.datastream_for_block(block_num);
         return read_block_id(ds, version, block_num);
      }
      return {};
   }

   std::unique_ptr<signed_block> block_log::read_signed_block_by_num(uint32_t block_num) const {
      return my->read_block_by_num(block_num);
   }

   block_id_type block_log::read_block_id_by_num(uint32_t block_num) const {
      return my->read_block_id_by_num(block_num);
   }

   uint64_t detail::block_log_impl::get_block_pos(uint32_t block_num) {
      if (!(head && block_num <= head->block_num() && block_num >= preamble.first_block_num))
         return block_log::npos;
      index_file.seek(sizeof(uint64_t) * (block_num - preamble.first_block_num));
      uint64_t pos;
      index_file.read((char*)&pos, sizeof(pos));
      return pos;
   }

   void detail::block_log_impl::read_head() {
      uint64_t pos;

      block_file.seek_end(-sizeof(pos));
      block_file.read((char*)&pos, sizeof(pos));
      if (pos != block_log::npos) {
         block_file.seek(pos);
         head = read_block(block_file, preamble.version);
      }
   }

   const signed_block_ptr& block_log::head() const {
      return my->head;
   }

   uint32_t block_log::first_block_num() const {
      if (!my->catalog.empty())
         return my->catalog.collection.begin()->first;
      return my->preamble.first_block_num;
   }

   void block_log::construct_index(const fc::path& block_file_name, const fc::path& index_file_name) {

      ilog("Will read existing blocks.log file ${file}", ("file", block_file_name.generic_string()));
      ilog("Will write new blocks.index file ${file}", ("file", index_file_name.generic_string()));

      block_log_data log_data(block_file_name);
      const uint32_t num_blocks = log_data.num_blocks();

      ilog("block log version= ${version}", ("version", log_data.version()));

      if (num_blocks == 0) {
         return;
      }

      ilog("first block= ${first}         last block= ${last}",
           ("first", log_data.first_block_num())("last", (log_data.last_block_num())));

      index_writer index(index_file_name, num_blocks);
      uint32_t     blocks_found = 0;

      for (auto iter = make_reverse_block_position_iterator(log_data);
           iter.get_value() != npos && blocks_found < num_blocks; ++iter, ++blocks_found) {
         index.write(iter.get_value());
      }

      EOS_ASSERT( blocks_found == num_blocks,
                  block_log_exception,
                  "Block log file at '${blocks_log}' formatting indicated last block: ${last_block_num}, first block: ${first_block_num}, but found ${num} blocks",
                  ("blocks_log", block_file_name.generic_string())("last_block_num", log_data.last_block_num())("first_block_num", log_data.first_block_num())("num", blocks_found));

   }

   static void write_incomplete_block_data(const fc::path& blocks_dir, fc::time_point now, uint32_t block_num, const char* start, int size) {
      auto tail_path = blocks_dir / std::string("blocks-bad-tail-").append(now).append(".log");
      fc::cfile tail;
      tail.set_file_path(tail_path);
      tail.open(fc::cfile::create_or_update_rw_mode);
      tail.write(start, size);

      ilog("Data at tail end of block log which should contain the (incomplete) serialization of block ${num} "
            "has been written out to '${tail_path}'.",
            ("num", block_num + 1)("tail_path", tail_path));
      
   }

   fc::path block_log::repair_log(const fc::path& data_dir, uint32_t truncate_at_block) {
      ilog("Recovering Block Log...");
      EOS_ASSERT(fc::is_directory(data_dir) && fc::is_regular_file(data_dir / "blocks.log"), block_log_not_found,
                 "Block log not found in '${blocks_dir}'", ("blocks_dir", data_dir));
                 
      if (truncate_at_block == 0)
         truncate_at_block = UINT32_MAX;

      auto now = fc::time_point::now();

      auto blocks_dir      = fc::canonical(data_dir); // canonical always returns an absolute path that has no symbolic link, dot, or dot-dot elements
      auto blocks_dir_name = blocks_dir.filename();
      auto backup_dir      = blocks_dir.parent_path() / blocks_dir_name.generic_string().append("-").append(now);

      EOS_ASSERT(!fc::exists(backup_dir), block_log_backup_dir_exist,
                 "Cannot move existing blocks directory to already existing directory '${new_blocks_dir}'",
                 ("new_blocks_dir", backup_dir));

      fc::rename(blocks_dir, backup_dir);
      ilog("Moved existing blocks directory to backup location: '${new_blocks_dir}'", ("new_blocks_dir", backup_dir));

      fc::create_directories(blocks_dir);
      const auto block_log_path  = blocks_dir / "blocks.log";
      const auto block_file_name = block_log_path.generic_string();

      ilog("Reconstructing '${new_block_log}' from backed up block log", ("new_block_log", block_file_name));

      block_log_data log_data;
      auto           ds  = log_data.open(backup_dir / "blocks.log");
      auto           pos = ds.tellp();
      std::string    error_msg;
      uint32_t       block_num = log_data.first_block_num() - 1;
      block_id_type  block_id;

      log_entry entry;
      if (log_data.version() < pruned_transaction_version) {
         entry.emplace<signed_block_v0>();
      }

      try {
         try {
            while (ds.remaining() > 0 && block_num < truncate_at_block) {
               std::tie(block_num, block_id) = block_log_data::full_validate_block_entry(ds, block_num, block_id, entry);
               if (block_num % 1000 == 0)
                  ilog("Verified block ${num}", ("num", block_num));
               pos  = ds.tellp();
            }
         }
         catch (const bad_block_excpetion& e) {
            write_incomplete_block_data(blocks_dir, now, block_num, log_data.data() + pos, log_data.size() - pos);
            std::rethrow_exception(e.inner);
         }
      } catch (const fc::exception& e) {
         error_msg = e.what();
      } catch (const std::exception& e) {
         error_msg = e.what();
      } catch (...) {
         error_msg = "unrecognized exception";
      }

      fc::cfile new_block_file;
      new_block_file.set_file_path(block_log_path);
      new_block_file.open(fc::cfile::create_or_update_rw_mode);
      new_block_file.write(log_data.data(), pos);

      if (error_msg.size()) {
         ilog("Recovered only up to block number ${num}. "
              "The block ${next_num} could not be deserialized from the block log due to error:\n${error_msg}",
              ("num", block_num)("next_num", block_num + 1)("error_msg", error_msg));
      } else if (block_num == truncate_at_block && pos < log_data.size()) {
         ilog("Stopped recovery of block log early at specified block number: ${stop}.", ("stop", truncate_at_block));
      } else {
         ilog("Existing block log was undamaged. Recovered all irreversible blocks up to block number ${num}.",
              ("num", block_num));
      }
      return backup_dir;
   }

   fc::optional<genesis_state> block_log::extract_genesis_state( const fc::path& block_dir ) {
      boost::filesystem::path p(block_dir / "blocks.log");
      for_each_file_in_dir_matches(block_dir, R"(blocks-1-\d+\.log)", [&p](boost::filesystem::path log_path) { p = log_path; });
      return block_log_data(p).get_genesis_state();
   }
      
   chain_id_type block_log::extract_chain_id( const fc::path& data_dir ) {
      return block_log_data(data_dir / "blocks.log").chain_id();
   }
   
   size_t block_log::prune_transactions(uint32_t block_num, std::vector<transaction_id_type>& ids) {
      try {

         EOS_ASSERT(my->preamble.version >= pruned_transaction_version, block_log_exception, 
                    "The block log version ${version} does not support transaction pruning.", ("version", my->preamble.version));
         const uint64_t pos = my->get_block_pos(block_num);
         EOS_ASSERT( pos != npos, block_log_exception,
                     "Specified block_num ${block_num} does not exist in block log.", ("block_num", block_num) );

         log_entry_v4 entry;   
         my->block_file.seek(pos);
         auto ds = my->block_file.create_datastream();
         unpack(ds, entry);

         EOS_ASSERT(entry.block.block_num() == block_num, block_log_exception,
                     "Wrong block was read from block log.");

         auto pruner = overloaded{[](transaction_id_type&) { return false; },
                                  [&ids](packed_transaction& ptx) {
                                     auto it = std::find(ids.begin(), ids.end(), ptx.id());
                                     if (it != ids.end()) {
                                        ptx.prune_all();
                                        // remove the found entry from ids
                                        ids.erase(it);
                                        return true;
                                     }
                                     return false;
                                  }};

         size_t num_trx_pruned = 0;
         for (auto& trx : entry.block.transactions) {
            num_trx_pruned += trx.trx.visit(pruner);
         }

         if (num_trx_pruned) {
            // we don't want to rewrite entire entry, just the block data itself.
            const auto block_offset = offset_to_block_start(my->preamble.version);
            my->block_file.seek(pos + block_offset);
            const uint32_t max_block_size = entry.meta.size - block_offset - sizeof(uint64_t);
            std::vector<char> buffer(max_block_size);
            fc::datastream<char*> stream(buffer.data(), buffer.size());
            entry.block.pack(stream, entry.meta.compression);
            my->block_file.write(buffer.data(), buffer.size());
            my->block_file.flush();
         }
         return num_trx_pruned;
      }
      FC_LOG_AND_RETHROW()
   }

   bool block_log::contains_genesis_state(uint32_t version, uint32_t first_block_num) {
      return version < genesis_state_or_chain_id_version || first_block_num == 1;
   }

   bool block_log::contains_chain_id(uint32_t version, uint32_t first_block_num) {
      return version >= genesis_state_or_chain_id_version && first_block_num > 1;
   }

   bool block_log::is_supported_version(uint32_t version) {
      return std::clamp(version, min_supported_version, max_supported_version) == version;
   }

   bool block_log::trim_blocklog_front(const fc::path& block_dir, const fc::path& temp_dir, uint32_t truncate_at_block) {
      EOS_ASSERT( block_dir != temp_dir, block_log_exception, "block_dir and temp_dir need to be different directories" );
      
      ilog("In directory ${dir} will trim all blocks before block ${n} from blocks.log and blocks.index.",
           ("dir", block_dir.generic_string())("n", truncate_at_block));

      block_log_archive archive(block_dir);

      if (truncate_at_block <= archive.log_data.first_block_num()) {
         dlog("There are no blocks before block ${n} so do nothing.", ("n", truncate_at_block));
         return false;
      }
      if (truncate_at_block > archive.log_data.last_block_num()) {
         dlog("All blocks are before block ${n} so do nothing (trim front would delete entire blocks.log).", ("n", truncate_at_block));
         return false;
      }

      // ****** create the new block log file and write out the header for the file
      fc::create_directories(temp_dir);
      fc::path new_block_filename = temp_dir / "blocks.log";
   
      static_assert( block_log::max_supported_version == pruned_transaction_version,
                     "Code was written to support format of version 4 or lower, need to update this code for latest format." );
      
      const auto     preamble_size           = block_log_preamble::nbytes_with_chain_id;
      const auto     num_blocks_to_truncate  = truncate_at_block - archive.log_data.first_block_num();
      const uint64_t first_kept_block_pos    = archive.log_index.nth_block_position(num_blocks_to_truncate);
      const uint64_t nbytes_to_trim          = first_kept_block_pos - preamble_size;
      const auto     new_block_file_size     = archive.log_data.size() - nbytes_to_trim;

      boost::iostreams::mapped_file_sink new_block_file;
      create_mapped_file(new_block_file, new_block_filename.generic_string(), new_block_file_size);
      fc::datastream<char*> ds(new_block_file.data(), new_block_file.size());

      block_log_preamble preamble;
      // version 4 or above have different log entry format; therefore version 1 to 3 can only be upgrade up to version 3 format.
      preamble.version         = archive.log_data.version() < pruned_transaction_version ? genesis_state_or_chain_id_version : block_log::max_supported_version;
      preamble.first_block_num = truncate_at_block;
      preamble.chain_context   = archive.log_data.chain_id();
      preamble.write_to(ds);

      memcpy(new_block_file.data() + preamble_size, archive.log_data.data() + first_kept_block_pos, new_block_file_size - preamble_size);

      fc::path new_index_filename = temp_dir / "blocks.index";
      index_writer index(new_index_filename, archive.log_index.num_blocks() - num_blocks_to_truncate);

      // walk along the block position of each block entry and decrement its value by nbytes_to_trim
      for (auto itr = make_reverse_block_position_iterator(new_block_file, preamble_size);
            itr.get_value() != block_log::npos; ++itr) {
         auto new_pos = itr.get_value() - nbytes_to_trim;
         index.write(new_pos);
         itr.set_value(new_pos);
      }

      index.close();
      new_block_file.close();

      fc::path old_log = temp_dir / "old.log";
      rename(archive.block_file_name, old_log);
      rename(new_block_filename, archive.block_file_name);
      fc::path old_ind = temp_dir / "old.index";
      rename(archive.index_file_name, old_ind);
      rename(new_index_filename, archive.index_file_name);

      return true;
   }

   int block_log::trim_blocklog_end(fc::path block_dir, uint32_t n) {       //n is last block to keep (remove later blocks)
      
      block_log_archive archive(block_dir);

      ilog("In directory ${block_dir} will trim all blocks after block ${n} from ${block_file} and ${index_file}",
         ("block_dir", block_dir.generic_string())("n", n)("block_file",archive.block_file_name.generic_string())("index_file", archive.index_file_name.generic_string()));

      if (n < archive.log_data.first_block_num()) {
         dlog("All blocks are after block ${n} so do nothing (trim_end would delete entire blocks.log)",("n", n));
         return 1;
      }
      if (n > archive.log_data.last_block_num()) {
         dlog("There are no blocks after block ${n} so do nothing",("n", n));
         return 2;
      }

      const auto to_trim_block_index    = n + 1 - archive.log_data.first_block_num();
      const auto to_trim_block_position = archive.log_index.nth_block_position(to_trim_block_index);
      const auto index_file_size        = to_trim_block_index * sizeof(uint64_t);

      boost::filesystem::resize_file(archive.block_file_name, to_trim_block_position);
      boost::filesystem::resize_file(archive.index_file_name, index_file_size);
      ilog("blocks.index has been trimmed to ${index_file_size} bytes", ("index_file_size", index_file_size));
      return 0;
   }

   void block_log::smoke_test(fc::path block_dir, uint32_t interval) {

      block_log_archive archive(block_dir);

      ilog("blocks.log and blocks.index agree on number of blocks");

      if (interval == 0) {
         interval = std::max((archive.log_index.num_blocks() + 7) >> 3, 1);
      }
      uint32_t expected_block_num = archive.log_data.first_block_num();

      for (auto pos_itr = archive.log_index.begin(); pos_itr < archive.log_index.end();
           pos_itr += interval, expected_block_num += interval) {
         archive.log_data.light_validate_block_entry_at(*pos_itr, expected_block_num);
      }
   }

   bool block_log::exists(const fc::path& data_dir) {
      return fc::exists(data_dir / "blocks.log") && fc::exists(data_dir / "blocks.index");
   }
}} /// eosio::chain
