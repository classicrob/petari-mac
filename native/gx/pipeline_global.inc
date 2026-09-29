static void petari_seed_global() {
  if (!PetariPipeline::globalPrecompile()) return;
  const char* directory = std::getenv("PETARI_PIPELINE_SEED_DIR");
  const auto root = directory && *directory ? std::filesystem::path(directory) :
      std::filesystem::path(g_config.resourcesPath ? g_config.resourcesPath : "") / "pipeline-seeds";
  const auto path = root / "__global__.db";
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return;
  const auto start = PetariPipeline::Clock::now();
  // Reuse the transaction/schema/version handling of the bundled startup seed.
  // Configs persist in the user DB; Dawn separately persists its translation cache.
  seed_pipeline_cache_path(path.string());
  std::fprintf(stderr, "[gx global prep] source=%s import_ms=%.3f\n", path.c_str(),
               PetariPipeline::milliseconds(PetariPipeline::Clock::now() - start));
}
