# ppmd-mini - standalone PPMd compressor

```
PPMDI 1.0
Usage: ppmdi [OPTION]... [FILE]...

  -c, --stdout       write on standard output, keep original files unchanged
  -d, --decompress   decompress
  -k, --keep         keep (don't delete) input files
  -f, --force        overwrite existing output files; allow terminal I/O
  -m, --memory=N     set memory size in MB (default 64)
  -o, --order=N      set model order (default 4)
  -v, --verbose      verbose mode
  -q, --quiet        suppress warnings
  -h, --help         give this help
  -V, --version      display version number
```

ppmdi is a data compression tool with command line syntax similar to gzip.
It uses the PPMd algorithm which provides superior compression for text files.
Specifically, it uses "variant I" of this algorithm which is also used in Zip.
PPMd was developed by Dmitry Shkarin; Igor Pavlov adapted his work for use in
the 7-Zip project. ppmdi employs Ppmd8 compression and decompression
routines from 7-Zip.  Files created by ppmd-mini are compatible with the
original PPMd implementation; they can also be decompressed with 7-Zip.
