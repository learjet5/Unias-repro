# Intro

This repository aims to reproduce and optimize the implementation of the paper "A Hybrid Alias Analysis and Its Application to Global Variable Protection in the Linux Kernel" (USENIX Security 2023).

## Environment

- [SVF-2.8](https://github.com/SVF-tools/SVF/releases/tag/SVF-2.8)
- [LLVM-14.0.6](https://github.com/llvm/llvm-project/releases/tag/llvmorg-14.0.6)

Note that: In order to better utilize the advantages of SVF, we choose the newer SVF version than the paper.

---

# Uasge

Compile Unias code:

```sh
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j 10
```

Use Unias to conduct alias analysis:

```sh
cd build
./bin/Unias @/path/to/bc.list -OutputDir=/path/to/output_dir -ThreadNum=8 2>&1 | tee runlog.txt
```

TBD

---

**Original Paper:** https://www.usenix.org/conference/usenixsecurity23/presentation/li-guoren

```
@inproceedings{lihybrid,
  title={A Hybrid Alias Analysis and Its Application to Global Variable Protection in the Linux Kernel},
  author={Li, Guoren and Zhang, Hang and Zhou, Jinmeng and Shen, Wenbo and Sui, Yulei and Qian, Zhiyun},
  booktitle={32st USENIX Security Symposium (USENIX Security 23)},
  pages={4211--4228},
  year={2023}
}
```