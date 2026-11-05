"""Reject damaged persistent descriptions before interpreting typed fields.
Uses Aurora's existing type-seeded content key and existing SQLite schema.
"""
from pathlib import Path
import sys

root=Path(sys.argv[1]);marker='GEKKOAOT_PIPELINE_INTEGRITY_V1'
updates={}
def edit(name,changes):
    path=root/name;source=path.read_text()
    if marker in source:return
    for old,new in changes:
        if source.count(old)!=1:raise RuntimeError(f'pipeline integrity anchor changed: {old[:100]}')
        source=source.replace(old,new)
    updates[path]='// '+marker+'\n'+source
edit('lib/gfx/pipeline_cache.cpp',[
        ('"SELECT config, first_frame_used FROM pipeline_cache "',
         '"SELECT config, first_frame_used, hash FROM pipeline_cache "'),
        ('    PipelineConfig config;\n    std::memcpy(&config, configBlob, sizeof(config));',
         '    const auto storedKey=static_cast<HashType>(sqlite3_column_int64(g_pipelineCacheLoadStmt,2));\n'
         '    const auto actualKey=xxh3_hash_s(configBlob,configSize,static_cast<HashType>(type));\n'
         '    if(storedKey!=actualKey){\n'
         '      std::fprintf(stderr,"GEKKOAOT_SHADER_CACHE_REJECT reason=content-key-mismatch type=%u key=%llu\\n",\n'
         '        unsigned(type),static_cast<unsigned long long>(storedKey));\n'
         '      continue; // Original live draws rebuild the description normally.\n'
         '    }\n'
         '    PipelineConfig config;\n    std::memcpy(&config, configBlob, sizeof(config));')])
for path,source in updates.items():path.write_text(source)
