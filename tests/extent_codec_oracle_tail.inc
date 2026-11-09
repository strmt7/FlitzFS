// Adapter only: primitive bodies above and composition admission below use the
// unchanged pinned upstream implementation. No upstream runtime/HIP is linked.
void reference_plain(BlockKind kind, std::uint8_t fill, std::span<const std::byte> input,
    std::span<std::byte> output) {
  switch (kind) {
    case BlockKind::Raw:
      if (input.size()!=output.size()) throw ArchiveError("oracle raw size");
      std::copy(input.begin(),input.end(),output.begin());return;
    case BlockKind::Fill:
      if (!input.empty()) throw ArchiveError("oracle fill size");
      std::fill(output.begin(),output.end(),static_cast<std::byte>(fill));return;
    case BlockKind::Pattern: materialize_pattern_cpu(input,output);return;
    case BlockKind::GpuPrefix: materialize_prefix_cpu(input,output);return;
    case BlockKind::GpuAdaptivePrefix: materialize_adaptive_prefix_cpu(input,output);return;
    case BlockKind::GpuDictionary: materialize_dictionary_cpu(input,output);return;
    case BlockKind::GpuSparsePattern: materialize_sparse_pattern_cpu(input,output,kMaxGpuPatternBytes);return;
    case BlockKind::GpuLongSparsePattern: materialize_sparse_pattern_cpu(input,output,kMaxGpuLongSparsePatternBytes);return;
    case BlockKind::GpuHuffman: materialize_huffman_cpu(input,output);return;
    default: throw ArchiveError("oracle stage is not plain");
  }
}
} // anonymous namespace
} // flitzzip

bool reference_native_decode(std::uint8_t kind, std::uint8_t fill,
    std::span<const std::byte> input, std::span<std::byte> output) {
  using namespace flitzzip;
  try {
    if (output.empty() || output.size()>kMaxArchiveBlockBytes || input.size()>=output.size()) return false;
    BlockDescriptor outer{static_cast<BlockKind>(kind),fill,static_cast<std::uint32_t>(output.size()),
        0,static_cast<std::uint32_t>(input.size())};
    if (kind==11) {
      const auto stages=parse_gpu_compound_block(input,outer);
      std::vector<std::byte> intermediate(stages.inner.uncompressed_len);
      reference_plain(stages.inner.kind,stages.inner.fill_value,stages.inner_payload,intermediate);
      reference_plain(stages.original.kind,0,intermediate,output);
    } else if (kind==12 || kind==13) {
      const auto stages=parse_gpu_byte_plane_stages(input,outer);
      std::vector<std::byte> transformed(output.size());
      std::size_t decoded=0;
      for (std::size_t i=0;i<stages.count;++i) {
        const auto& block=stages.blocks[i];
        reference_plain(block.kind,block.fill_value,
            stages.payload.subspan(static_cast<std::size_t>(block.encoded_offset),block.encoded_len),
            std::span<std::byte>(transformed).subspan(decoded,block.uncompressed_len));
        decoded+=block.uncompressed_len;
      }
      if (decoded!=output.size()) return false;
      const auto rows=output.size()/stages.width;
      for (std::size_t i=0;i<output.size();++i)
        output[i]=i<rows*stages.width ? transformed[(i%stages.width)*rows+i/stages.width] : transformed[i];
    } else reference_plain(outer.kind,fill,input,output);
    return true;
  } catch (const std::exception&) { return false; }
}
