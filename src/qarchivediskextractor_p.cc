#include <qarchivediskextractor_p.hpp>
#include <QFileInfo>
#include <QDateTime>
#include <QScopedPointer>
#include <QCoreApplication>
extern "C" {
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
}

using namespace QArchive;

#define DISK_EXTRACTOR_CONSTRUCTOR QObject(),\
				   _mInfo(QSharedPointer<QJsonObject>(new QJsonObject)), \
				   _mExtractFilters(QSharedPointer<QStringList>(new QStringList))

#if defined(__APPLE__)
#define st_atim st_atimespec.tv_sec
#define st_ctim st_ctimespec.tv_sec
#define st_mtim st_mtimespec.tv_sec
#elif defined(_WIN32) && !defined(__CYGWIN__)
#define st_atim st_atime
#define st_ctim st_ctime
#define st_mtim st_mtime
#else
#define st_atim st_atim.tv_sec
#define st_ctim st_ctim.tv_sec
#define st_mtim st_mtim.tv_sec
#endif

#define PASSWORD_NEEDED(a) !strcmp(archive_error_string(a.data()) , "Passphrase required for this entry")
#define PASSWORD_INCORRECT(a) !strcmp(archive_error_string(a.data()) , "Incorrect passphrase")

static QString getDirectoryFileName(const QString &dir)
{
    if(dir[dir.count() - 1] == QStringLiteral("/")) {
        return dir.mid(0, dir.count() - 1);
    }
    return dir;
}

struct ScopedPointerArchiveReadDeleter
{
    static inline void cleanup(struct archive *ar)
    {
       if(ar) {
	       archive_read_close(ar);
	       archive_read_free(ar);
       }
       return;
    }
};

struct ScopedPointerArchiveWriteDeleter
{
    static inline void cleanup(struct archive *aw)
    {
	    if(aw) {
		    archive_write_close(aw);
		    archive_write_free(aw);
	    }
	    return;
    }
};

struct ScopedPointerArchiveEntryDeleter
{
    static inline void cleanup(struct archive_entry *entry){
	    if(entry){
		    archive_entry_free(entry);
	    }
	    return;
    }
};

static void ignoreDeleteQFile(QFile *file)
{
	(void)file;
	return;
}

DiskExtractorPrivate::DiskExtractorPrivate()
	: DISK_EXTRACTOR_CONSTRUCTOR
{
}

DiskExtractorPrivate::DiskExtractorPrivate(QFile *inputArchive)
	: DISK_EXTRACTOR_CONSTRUCTOR
{
	setArchive(inputArchive);
}

DiskExtractorPrivate::DiskExtractorPrivate(const QString &inputArchivePath)
	: DISK_EXTRACTOR_CONSTRUCTOR
{
	setArchive(inputArchivePath);
}

DiskExtractorPrivate::~DiskExtractorPrivate()
{
	_mArchive.clear();
	_mExtractFilters.clear();
}


void DiskExtractorPrivate::setArchive(QFile *archive)
{
    clear(); /* clear old data. */
    if(archive == nullptr) {
	emit error(InvalidQFile);
        return;
    }
    _mArchive = QSharedPointer<QFile>(archive, ignoreDeleteQFile);
    
    /* Check if exists */
    if(!_mArchive->exists()) {
        _mArchive.clear();
        emit error(ArchiveDoesNotExists);
	return;
    }

    /* Check if readable. */
    if(!_mArchive->isReadable()) {
        _mArchive.clear(); /* delete everything */
        emit error(ArchiveIsNotReadable);
	return;
    }

    /* Check if opened. */
    if(!_mArchive->isOpen()) {
        _mArchive.clear(); /* delete everything */
       	emit error(ArchiveIsNotOpened);	
	return;
    }
    return;
}

void DiskExtractorPrivate::setArchive(const QString &archivePath)
{
    clear(); /* clear old data */
    if(archivePath.isEmpty()) {
        return;
    }

    try {
        _mArchive = QSharedPointer<QFile>(new QFile);
    } catch ( ... ) {
        emit error(NotEnoughMemory);
        throw;
    }

    /*
     * Check if its really a file and not a folder.
    */
    if(!QFileInfo(archivePath).isFile()) {
        _mArchive.clear();
        emit error(ArchiveDoesNotExists);
	return;
    }

    _mArchive->setFileName(archivePath);

    /* Check if the file actually exists. */
    if(!_mArchive->exists()) {
        _mArchive.clear(); /* delete everything to avoid futher errors. */
        emit error(ArchiveDoesNotExists);
	return;
    }

    /* Check if we have the permission to read it. */
    auto perm = _mArchive->permissions();
    if(
        !(perm & QFileDevice::ReadUser) &&
        !(perm & QFileDevice::ReadGroup) &&
        !(perm & QFileDevice::ReadOther)
    ) {
        _mArchive.clear();
        emit error(NoPermissionToReadArchive);
	return;
    }

    /*
     * Finally open the file.
    */
    if(!_mArchive->open(QIODevice::ReadOnly)) {
        _mArchive.clear();
        emit error(CannotOpenArchive);
	return;
    }
    return;
}

void DiskExtractorPrivate::setBlockSize(int n)
{
	_nBlockSize = n;
	return;
}

void DiskExtractorPrivate::setOutputDirectory(const QString &destination)
{
    /* Check if its a directory and not a file , Also check if it exists. */
    if(destination.isEmpty() || !QFileInfo(destination).isDir() || !QFileInfo::exists(destination)) {
        emit error(InvalidOutputDirectory);
	return;
    }
    _mOutputDirectory = destination;
    return;
}


void DiskExtractorPrivate::setPassword(const QString &passwd)
{
	if(passwd.isEmpty()){
		return;
	}
	_mPassword = passwd;
	return;
}

void DiskExtractorPrivate::addFilter(const QString &filter)
{
	if(filter.isEmpty()){
		return;
	}
	*(_mExtractFilters.data()) << filter;
	return;
}

void DiskExtractorPrivate::addFilter(const QStringList &filters)
{
	if(filters.isEmpty()){
		return;
	}
	*(_mExtractFilters.data()) << filters;
	return;
}

void DiskExtractorPrivate::clear()
{
	_mInfo.clear();
	_mInfo = QSharedPointer<QJsonObject>(new QJsonObject);
	_mExtractFilters->clear();
	_mPassword.clear();
	_mOutputDirectory.clear();
	return;
}

void DiskExtractorPrivate::getInfo()
{
	int ret = processArchiveInformation();
	if(!ret)
		emit info(*(_mInfo.data()));
	else if(ret == -4)
		emit getInfoRequirePassword(_nPasswordTriedCountGetInfo);
	return;
}

int DiskExtractorPrivate::processArchiveInformation()
{
    if(_mArchive.isNull()){
	    return -1;
    }

    /*
     * Reset any previous operations 
     * on the file.
    */
    _mArchive->seek(0);

    int ret = 0;
    archive_entry *entry = nullptr;
    QScopedPointer<struct archive , ScopedPointerArchiveReadDeleter> inArchive(archive_read_new());
    if(!_mPassword.isEmpty()){
	    archive_read_add_passphrase(inArchive.data() , _mPassword.toLatin1().constData());
    }
    if(!(inArchive.data())){
	    emit error(NotEnoughMemory);
	    return -2;
    }
    archive_read_support_format_all(inArchive.data());
    archive_read_support_filter_all(inArchive.data());
    if((ret = archive_read_open_fd(inArchive.data(), _mArchive->handle() ,_nBlockSize))) {
        error(ArchiveReadError);
	return -3;
    }
    for (;;) {
        ret = archive_read_next_header(inArchive.data(), &entry);
        if (ret == ARCHIVE_EOF){
            break;
        }
        if (ret != ARCHIVE_OK) {
            if(PASSWORD_NEEDED(inArchive) || PASSWORD_INCORRECT(inArchive)){
		return -4;		
	    }

	    emit error(ArchiveCorrupted);
	    return -5;
        }
        QString CurrentFile = QString(archive_entry_pathname(entry));
   	QJsonObject CurrentEntry;
	auto entry_stat = archive_entry_stat(entry);
	qint64 size = (qint64)entry_stat->st_size;
	QString sizeUnits = "Bytes";
	if(size == 0) {
        sizeUnits = "None";
        size = 0;
	} else if(size < 1024) {
        sizeUnits = "Bytes";
        size = size;
	} else if(size >= 1024 && size < 1048576) {
        sizeUnits = "KiB";
        size /= 1024;
	} else if(size >= 1048576 && size < 1073741824) {
        sizeUnits = "MiB";
        size /= 1048576;
	} else {
        sizeUnits = "GiB";
        size /= 1073741824;
	}

    	// MSVC (and maybe Windows in general?) workaround
#if defined(_WIN32) && !defined(__CYGWIN__)
    qint64 blockSizeInBytes = 512;
    qint64 blocks = (qint64) ceil(entry_stat->st_size / blockSizeInBytes);
#else
    qint64 blockSizeInBytes = (qint64)entry_stat->st_blksize;
    qint64 blocks = (qint64)entry_stat->st_blocks;
#endif
    auto lastAccessT = entry_stat->st_atim;
    auto lastModT = entry_stat->st_mtim;
    auto lastStatusModT = entry_stat->st_ctim; 
    
    QFileInfo fileInfo(CurrentFile);

    auto ft = archive_entry_filetype(entry);
    QString FileType;
    switch(ft) {
    case AE_IFREG: // Regular file
        FileType = "RegularFile";
        break;
    case AE_IFLNK: // Link
        FileType = "SymbolicLink";
        break;
    case AE_IFSOCK: // Socket
        FileType = "Socket";
        break;
    case AE_IFCHR: // Character Device
        FileType = "CharacterDevice";
        break;
    case AE_IFBLK: // Block Device
        FileType = "BlockDevice";
        break;
    case AE_IFDIR: // Directory.
        FileType = "Directory";
        break;
    case AE_IFIFO: // Named PIPE. (fifo)
        FileType = "NamedPipe";
        break;
    default:
        FileType = "UnknownFile";
        break;
    };
    // Set the values.
    if(FileType != "RegularFile") {
        CurrentEntry.insert("FileName", getDirectoryFileName(CurrentFile));
    } else {
        CurrentEntry.insert("FileName", fileInfo.fileName());
    }

    CurrentEntry.insert("FileType", QJsonValue(FileType));
    CurrentEntry.insert("Size", QJsonValue(size));
    CurrentEntry.insert("SizeUnit", sizeUnits);
    CurrentEntry.insert("BlockSize", QJsonValue(blockSizeInBytes));
    CurrentEntry.insert("BlockSizeUnit", "Bytes");
    CurrentEntry.insert("Blocks", QJsonValue(blocks));
    if(lastAccessT) {
        CurrentEntry.insert("LastAccessedTime", QJsonValue((QDateTime::fromTime_t(lastAccessT)).toString(Qt::ISODate)));
    } else {
        CurrentEntry.insert("LastAccessedTime", "Unknown");
    }

    if(lastModT) {
        CurrentEntry.insert("LastModifiedTime", QJsonValue((QDateTime::fromTime_t(lastModT)).toString(Qt::ISODate)));
    } else {
        CurrentEntry.insert("LastModifiedTime", "Unknown");
    }

    if(lastStatusModT) {
        CurrentEntry.insert("LastStatusModifiedTime", QJsonValue((QDateTime::fromTime_t(lastStatusModT)).toString(Qt::ISODate)));
    } else {
        CurrentEntry.insert("LastStatusModifiedTime", "Unknown");
    }
    _mInfo->insert(CurrentFile , CurrentEntry);
    QCoreApplication::processEvents();
    }
    return 0;
}