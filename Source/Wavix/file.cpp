#include "Inline/BasicTypes.h"
#include "Inline/Unicode.h"
#include "Platform/Platform.h"
#include "Runtime/Intrinsics.h"
#include "wavix.h"

static bool validateFD(I32 index)
{
	return index >= 0 && Uptr(index) < currentThread->process->files.size();
}

static I32 allocateFD()
{
	if(currentThread->process->freeFileIndices.size())
	{
		const I32 result = currentThread->process->freeFileIndices.back();
		currentThread->process->freeFileIndices.pop_back();
		return result;
	}
	else
	{
		if(currentThread->process->files.size() >= Uptr(INT32_MAX)) { return 0; }
		currentThread->process->files.push_back(nullptr);
		return I32(currentThread->process->files.size() - 1);
	}
}

static void freeFD(I32 index)
{
	assert(validateFD(index));
	currentThread->process->freeFileIndices.push_back(index);
}

struct Path
{
	enum class Base
	{
		cwd,
		home,
		root,
	};

	Base base;
	std::vector<std::string> components;
};

static bool parsePath(const std::string& pathString,Path& outPath)
{
	outPath.components.clear();
	if(pathString.size() == 0) { return false; }

	const U8* nextChar = (const U8*)pathString.c_str();
	const U8* endChar = nextChar + pathString.size();

	switch(*nextChar)
	{
	case '/': outPath.base = Path::Base::root; ++nextChar; break;
	case '~': outPath.base = Path::Base::home; ++nextChar; break;
	default: outPath.base = Path::Base::cwd; break;
	};

	std::string nextComponent;
	while(nextChar != endChar)
	{
		switch(*nextChar)
		{
		case '/':
		{
			++nextChar;
			if(nextComponent.size()) { outPath.components.emplace_back(std::move(nextComponent)); }
			break;
		}
		default:
		{
			U32 codePoint = 0;
			const U8* codePointStart = nextChar;
			if(!Unicode::decodeUTF8CodePoint(nextChar,endChar,codePoint)) { return false; }
			while(codePointStart < nextChar) { nextComponent += *codePointStart++; }
			break;
		}
		};
	};

	if(nextComponent.size()) { outPath.components.emplace_back(std::move(nextComponent)); }

	return true;
}

#define PATH_SEPARATOR '/'

static std::string resolvePath(
	const std::string& cwd,
	const std::string& home,
	const Path& path)
{
	std::string result;
	switch(path.base)
	{
	case Path::Base::cwd: result = cwd; break;
	case Path::Base::home: result = home; break;
	default: break;
	};

	for(const std::string& component : path.components)
	{
		result += PATH_SEPARATOR;
		result += component;
	}

	return result;
}

namespace OpenFlags
{
	enum
	{
		readOnly = 0x0000,
		writeOnly = 0x0001,
		readWrite = 0x0002,
		accessModeMask = 0x0003,

		create = 0x0040,
		exclusive = 0x0080,
		truncate = 0x0200,
		createModeMask = create | exclusive | truncate,

		//noCTTY = 0x0100,
		//append = 0x0400,
		//nonBlocking = 0x0800,
	};
};

static std::string readPathString(I32 pathAddress)
{
	// Validate the path name and make a local copy of it.
	std::string pathString;
	while(true)
	{
		const char c = memoryRef<char>(currentThread->process->memory,pathAddress + pathString.size());
		if(c == 0) { break; }
		else { pathString += c; }
	};

	return pathString;
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_open",I32,__syscall_open,
	I32 pathAddress, I32 flags, I32 mode)
{
	traceSyscallf("open","");

	std::string pathString = readPathString(pathAddress);
	Path path;
	if(!parsePath(pathString,path)) { return -1; }

	std::string cwd;
	{
		Platform::Lock cwdLock(currentThread->process->cwdMutex);
		cwd = currentThread->process->cwd;
	}
	pathString = sysroot + resolvePath(cwd,"/home",path);

	// Validate and interpret the access mode.
	Platform::FileAccessMode platformAccessMode;
	const I32 accessMode = flags & OpenFlags::accessModeMask;
	switch(accessMode)
	{
	case OpenFlags::readOnly: platformAccessMode = Platform::FileAccessMode::readOnly; break;
	case OpenFlags::writeOnly: platformAccessMode = Platform::FileAccessMode::writeOnly; break;
	case OpenFlags::readWrite: platformAccessMode = Platform::FileAccessMode::readWrite; break;
	default:
		return -1;
	};

	Platform::FileCreateMode platformCreateMode;
	if(flags & OpenFlags::create)
	{
		if(flags & OpenFlags::exclusive)
		{
			platformCreateMode = Platform::FileCreateMode::createNew;
		}
		else if(flags & OpenFlags::truncate)
		{
			platformCreateMode = Platform::FileCreateMode::createAlways;
		}
		else
		{
			platformCreateMode = Platform::FileCreateMode::openAlways;
		}
	}
	else
	{
		if(flags & OpenFlags::truncate)
		{
			platformCreateMode = Platform::FileCreateMode::truncateExisting;
		}
		else
		{
			platformCreateMode = Platform::FileCreateMode::openExisting;
		}
	}

	Platform::File* platformFile = Platform::openFile(pathString,platformAccessMode,platformCreateMode);
	if(!platformFile) { return -1; }

	Platform::Lock fileLock(currentThread->process->fileMutex);
	I32 fd = allocateFD();
	currentThread->process->files[fd] = platformFile;

	return fd;
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_openat",I32,__syscall_openat,
	I32 dirfd, I32 pathName, I32 flags, I32 mode)
{
	traceSyscallf("openat","");
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_creat",I32,__syscall_creat,
	I32 pathName, I32 mode)
{
	traceSyscallf("creat","");
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_close",I32,__syscall_close,I32 fd)
{
	traceSyscallf("close","");

	Platform::Lock fileLock(currentThread->process->fileMutex);

	if(!validateFD(fd)) { return -1; }

	Platform::File* platformFile = currentThread->process->files[fd];
	freeFD(fd);

	if(Platform::closeFile(platformFile)) { return 0; }
	else { return -1; }
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_llseek",I32,__syscall_llseek,
	I32 fd, I32 offsetHigh, I32 offsetLow, I32 resultAddress, I32 whence)
{
	traceSyscallf("llseek","");

	const I64 offset = I64(U64(offsetHigh) << 32) | I64(offsetLow);

	if(whence < 0 || whence > 2) { return -1; }

	Platform::Lock fileLock(currentThread->process->fileMutex);

	if(!validateFD(fd)) { return -1; }

	Platform::File* platformFile = currentThread->process->files[fd];

	if(!Platform::seekFile(
		platformFile,
		offset,
		(Platform::FileSeekOrigin)whence,
		memoryRef<U64>(currentThread->process->memory,resultAddress)))
	{
		return -1;
	}

	return 0;
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_read",I32,__syscall_read,
	I32 fd, I32 bufferAddress, I32 numBytes)
{
	traceSyscallf("read","");

	Platform::Lock fileLock(currentThread->process->fileMutex);

	if(!validateFD(fd)) { return -1; }

	Platform::File* platformFile = currentThread->process->files[fd];
	if(!platformFile)
	{
		throwException(Exception::calledUnimplementedIntrinsicType);
	}

	U8* buffer = memoryArrayPtr<U8>(currentThread->process->memory,bufferAddress,numBytes);

	Uptr numReadBytes = 0;
	const bool result = Platform::readFile(platformFile,buffer,numBytes,numReadBytes);
	if(!result)
	{
		return -1;
	}

	return coerce32bitAddress(numReadBytes);
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_readv",I32,__syscall_readv,
	I32 fd, I32 iosAddress, I32 numIos)
{
	traceSyscallf("readv","");

	struct IoVec
	{
		U32 address;
		U32 numBytes;
	};

	Platform::Lock fileLock(currentThread->process->fileMutex);

	if(!validateFD(fd)) { return -1; }

	Platform::File* platformFile = currentThread->process->files[fd];
	if(!platformFile)
	{
		throwException(Exception::calledUnimplementedIntrinsicType);
	}

	const IoVec* ios = memoryArrayPtr<const IoVec>(currentThread->process->memory,iosAddress,numIos);
	Uptr numReadBytes = 0;
	for(U32 ioIndex = 0; ioIndex < U32(numIos); ++ioIndex)
	{
		const IoVec io = ios[ioIndex];
		if(io.numBytes)
		{
			U8* ioData = memoryArrayPtr<U8>(currentThread->process->memory,io.address,io.numBytes);
			Uptr ioNumReadBytes = 0;
			const bool ioResult = Platform::readFile(platformFile,ioData,io.numBytes,ioNumReadBytes);
			numReadBytes += ioNumReadBytes;
			if(!ioResult || ioNumReadBytes != io.numBytes)
			{
				break;
			}
		}
	}

	return coerce32bitAddress(numReadBytes);
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_writev",I32,__syscall_writev,
	I32 fd, I32 iosAddress, I32 numIOs)
{
	traceSyscallf("writev","");

	struct IoVec
	{
		U32 address;
		U32 numBytes;
	};

	Platform::Lock fileLock(currentThread->process->fileMutex);

	if(!validateFD(fd)) { return -1; }

	Platform::File* platformFile = currentThread->process->files[fd];
	if(!platformFile)
	{
		throwException(Exception::calledUnimplementedIntrinsicType);
	}

	const IoVec* ios = memoryArrayPtr<const IoVec>(currentThread->process->memory,iosAddress,numIOs);
	Uptr numWrittenBytes = 0;
	for(U32 ioIndex = 0; ioIndex < U32(numIOs); ++ioIndex)
	{
		const IoVec io = ios[ioIndex];
		if(io.numBytes)
		{
			const U8* ioData = memoryArrayPtr<U8>(currentThread->process->memory,io.address,io.numBytes);
			Uptr ioNumWrittenBytes = 0;
			const bool ioResult = Platform::writeFile(platformFile,ioData,io.numBytes,ioNumWrittenBytes);
			numWrittenBytes += ioNumWrittenBytes;
			if(!ioResult || ioNumWrittenBytes != io.numBytes)
			{
				break;
			}
		}
	}

	return coerce32bitAddress(numWrittenBytes);
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_fsync",I32,__syscall_fsync,I32 fd)
{
	traceSyscallf("fsync","");

	Platform::Lock fileLock(currentThread->process->fileMutex);

	if(!validateFD(fd)) { return -1; }

	Platform::File* platformFile = currentThread->process->files[fd];
	if(!platformFile)
	{
		throwException(Exception::calledUnimplementedIntrinsicType);
	}

	Platform::flushFileWrites(platformFile);

	return 0;
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_fdatasync",I32,__syscall_fdatasync,I32 fd)
{
	traceSyscallf("fdatasync","");

	Platform::Lock fileLock(currentThread->process->fileMutex);

	if(!validateFD(fd)) { return -1; }

	Platform::File* platformFile = currentThread->process->files[fd];
	if(!platformFile)
	{
		throwException(Exception::calledUnimplementedIntrinsicType);
	}

	Platform::flushFileWrites(platformFile);

	return 0;
}

#define WAVIX_F_DUPFD 0
#define WAVIX_F_GETFD 1
#define WAVIX_F_SETFD 2
#define WAVIX_F_GETFL 3
#define WAVIX_F_SETFL 4
#define WAVIX_F_GETLK 5
#define WAVIX_F_SETLK 6
#define WAVIX_F_SETLKW 7
#define WAVIX_F_GETOWN 9
#define WAVIX_F_SETOWN 8
#define WAVIX_FD_CLOEXEC 1
#define WAVIX_F_RDLCK 0
#define WAVIX_F_UNLCK 2
#define WAVIX_F_WRLCK 1
#define WAVIX_SEEK_SET 0
#define WAVIX_SEEK_CUR 1
#define WAVIX_SEEK_END 2
#define WAVIX_O_CREAT 0100
#define WAVIX_O_EXCL 0200
#define WAVIX_O_NOCTTY 0400
#define WAVIX_O_TRUNC 01000
#define WAVIX_O_APPEND 02000
#define WAVIX_O_DSYNC 010000
#define WAVIX_O_NONBLOCK 04000
#define WAVIX_O_RSYNC 04010000
#define WAVIX_O_SYNC 04010000
#define WAVIX_O_ACCMODE 010000030
#define WAVIX_O_RDONLY 00
#define WAVIX_O_RDWR 02
#define WAVIX_O_WRONLY 01

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_fcntl64",I32,__syscall_fcntl64, 
	I32 fd, I32 cmd, I32 arg)
{
	traceSyscallf("fnctl64","(%i,%i,%i)",fd,cmd,arg);
	switch(cmd)
	{
	case WAVIX_F_GETFL: return 0;
	case WAVIX_F_SETFL: return 0;
	case WAVIX_F_SETFD: return 0;
	default:
		throwException(Exception::calledUnimplementedIntrinsicType);
	}
}

typedef I32 wavix_uid_t;
typedef I32 wavix_gid_t;

typedef U32 wavix_mode_t;
typedef U32 wavix_nlink_t;
typedef I64 wavix_off_t;
typedef U64 wavix_ino_t;
typedef U64 wavix_dev_t;
typedef I32 wavix_blksize_t;
typedef I64 wavix_blkcnt_t;
typedef U64 wavix_fsblkcnt_t;
typedef U64 wavix_fsfilcnt_t;

struct wavix_stat
{
	wavix_dev_t st_dev;
	I32 __st_dev_padding;
	I32 __st_ino_truncated;
	wavix_mode_t st_mode;
	wavix_nlink_t st_nlink;
	wavix_uid_t st_uid;
	wavix_gid_t st_gid;
	wavix_dev_t st_rdev;
	I32 __st_rdev_padding;
	wavix_off_t st_size;
	wavix_blksize_t st_blksize;
	wavix_blkcnt_t st_blocks;
	wavix_timespec st_atim;
	wavix_timespec st_mtim;
	wavix_timespec st_ctim;
	wavix_ino_t st_ino;
};

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_stat64",I32,__syscall_stat64,
	I32 pathAddress, I32 resultAddress)
{
	std::string pathString = readPathString(pathAddress);
	Path path;
	if(!parsePath(pathString,path)) { return -1; }

	traceSyscallf("stat64","(\"%s\",0x%08x)",pathString.c_str(),resultAddress);

	wavix_stat& result = memoryRef<wavix_stat>(currentThread->process->memory,resultAddress);
	memset(&result,0,sizeof(wavix_stat));

	return 0;
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_lstat64",I32,__syscall_lstat64,
	I32 pathAddress, I32 resultAddress)
{
	std::string pathString = readPathString(pathAddress);
	Path path;
	if(!parsePath(pathString,path)) { return -1; }

	traceSyscallf("lstat64","(\"%s\",0x%08x)",pathString.c_str(),resultAddress);

	wavix_stat& result = memoryRef<wavix_stat>(currentThread->process->memory,resultAddress);
	memset(&result,0,sizeof(wavix_stat));

	return 0;
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_fstat64",I32,__syscall_fstat64,
	I32 fd, I32 resultAddress)
{
	traceSyscallf("fstat64","(%i,0x%08x)",fd,resultAddress);

	wavix_stat& result = memoryRef<wavix_stat>(currentThread->process->memory,resultAddress);
	memset(&result,0,sizeof(wavix_stat));

	return 0;
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_faccessat",I32,__syscall_faccessat,
	I32 dirfd, I32 pathAddress, I32 mode, I32 flags)
{
	std::string pathString = readPathString(pathAddress);
	Path path;
	if(!parsePath(pathString,path)) { return -1; }

	traceSyscallf("faccessat","(%i,\"%s\",%u,%u)",dirfd,pathString.c_str(),mode,flags);
	return -1;
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_unlink",I32,__syscall_unlink,I32 a)
{
	traceSyscallf("unlink","(%i)",a);
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_chdir",I32,__syscall_chdir,
	I32 pathAddress)
{
	std::string pathString = readPathString(pathAddress);
	Path path;
	if(!parsePath(pathString,path)) { return -1; }

	traceSyscallf("chdir","(\"%s\")",pathString.c_str());

	Platform::Lock cwdLock(currentThread->process->cwdMutex);
	currentThread->process->cwd = resolvePath(currentThread->process->cwd,"/home",path);

	return 0;
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_access",I32,__syscall_access,I32 a,I32 b)
{
	traceSyscallf("access","(%i,%i)",a,b);
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_dup",I32,__syscall_dup,I32 a)
{
	traceSyscallf("dup","(%i)",a);
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_dup2",I32,__syscall_dup2,I32 a,I32 b)
{
	traceSyscallf("dup2","(%i,%i)",a,b);
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_readlink",I32,__syscall_readlink,
	I32 pathAddress, I32 bufferAddress, U32 numBufferBytes)
{
	traceSyscallf("readlink","(%i,%i,%i)",a,b,c);
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_umask",I32,__syscall_umask,I32 a)
{
	traceSyscallf("umask","(%i,%i)",a);
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_rename",I32,__syscall_rename,I32 a,I32 b)
{
	traceSyscallf("rename","(%i,%i)",a,b);
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_chown32",I32,__syscall_chown32,I32 a,I32 b,I32 c)
{
	traceSyscallf("chown32","(%i,%i,%i)",a,b,c);
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_getdents64",I32,__syscall_getdents64,I32 a,I32 b,I32 c)
{
	traceSyscallf("getdents64","(%i,%i,%i)",a,b,c);
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_pipe",I32,__syscall_pipe,I32 a)
{
	traceSyscallf("pipe","(%i)",a);
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_poll",I32,__syscall_poll,I32 a,I32 b,I32 c)
{
	traceSyscallf("poll","(%i,%i,%i)",a,b,c);
	throwException(Exception::calledUnimplementedIntrinsicType);
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall_pselect6",I32,__syscall_pselect6, 
	I32 a, I32 b, I32 c, I32 d, I32 e, I32 f)
{
	traceSyscallf("pselect","(%i,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x)",a,b,c,d,e,f);
	//throwException(Exception::calledUnimplementedIntrinsicType);
	return 0;
}

DEFINE_INTRINSIC_FUNCTION(wavix,"__syscall__newselect",I32,__syscall__newselect,
	I32 a, I32 b, I32 c, I32 d, I32 e)
{
	traceSyscallf("_newselect","(%i,0x%08x,0x%08x,0x%08x,0x%08x)",a,b,c,d,e);
	//throwException(Exception::calledUnimplementedIntrinsicType);
	return 0;
}

#define TIOCGWINSZ	0x5413

DEFINE_INTRINSIC_FUNCTION_WITH_MEM_AND_TABLE(wavix,"__syscall_ioctl",I32,__syscall_ioctl,
	I32 fd, I32 request, I32 arg0, I32 arg1, I32 arg2, I32 arg3)
{
	traceSyscallf("ioctl","");
	switch(request)
	{
	case TIOCGWINSZ:
	{
		struct WinSize
		{
			unsigned short ws_row;		/* rows, in characters */
			unsigned short ws_col;		/* columns, in characters */
			unsigned short ws_xpixel;	/* horizontal size, pixels */
			unsigned short ws_ypixel;	/* vertical size, pixels */
		};

		WinSize& winSize = memoryRef<WinSize>(currentThread->process->memory,arg0);
		winSize.ws_row = 43;
		winSize.ws_col = 80;
		winSize.ws_xpixel = 800;
		winSize.ws_ypixel = 600;
		return 0;
	}
	default:
		return EINVAL;
	};
}

void staticInitializeFile()
{
}