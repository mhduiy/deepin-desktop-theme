#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QDateTime>
#include <QTextStream>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QtConcurrent>
#include <QMutex>
#include <QObject>
#include <QMap>
#include <qcontainerfwd.h>
#include <qdir.h>
#include <qlogging.h>

// 转换任务结构
struct ConvertTask {
    QString sourceFile;
    QString relativePath;
};

// 多尺寸转换任务结构
struct MultiSizeConvertTask {
    QString iconName;
    QStringList sourceFiles;  // 不同尺寸的源文件列表
    QStringList sizes;        // 对应的尺寸列表
};

class HicolorConverter {
public:
    HicolorConverter();
    ~HicolorConverter();
    
    bool initialize();
    int run();

private:
    // 配置变量
    QString m_sourceDir;
    QString m_targetDir;
    QString m_recordFile;
    QString m_logFile;
    QStringList m_appDirs;
    QString m_dciTool;
    
    // 运行时变量
    QFile m_logFileHandle;
    QTextStream m_logStream;
    
    // 统计计数
    int m_totalConverted;
    int m_totalSkipped;
    int m_totalFailed;
    
    // 线程安全
    QMutex m_logMutex;
    QMutex m_recordMutex;
    
    // 内存中的记录缓存
    QMap<QString, QString> m_recordCache;  // iconName -> hash
    bool m_recordCacheLoaded = false;
    bool m_recordCacheModified = false;

    // 目录扫描缓存
    struct DirectoryCache {
        QStringList sizeDirectories;  // 所有尺寸目录 (如 16x16/apps, 24x24/apps)
        QStringList appDirectories;   // 所有应用目录 (如 /apps, scalable/apps)
        QMap<QString, QStringList> iconFilesByDir;  // 每个目录下的图标文件
        QSet<QString> allIconNames;   // 所有图标名称
        bool isInitialized = false;
    };
    DirectoryCache m_dirCache;
    
    // 支持的上下文类型（方便扩展）
    QStringList m_supportedContexts = {"apps"};  // 目前只支持apps，后续可扩展
    
    // 图标转换优先级列表（越靠前优先级越高）
    // 若前面优先级的图标已经处理，则后面的无需处理
    QStringList m_iconPriorities = {
        // 多尺寸图标（*x*/apps）优先级最高，使用批量转换
        // 这些会在处理多尺寸图标时统一处理，不在此列表中
        "scalable/apps",    // 最高优先级
        "symbolic/apps",    // 第二优先级
        "apps",            // 第三优先级
        // 其余路径的图标按默认排序处理
    };

    // 核心功能
    bool checkDciTool();
    bool createDirectories();
    void initializeDirectoryCache();

    // 记录缓存管理
    void loadRecordCache();
    void flushRecordCache();

    void logMessage(const QString &message);
    QString getFileHash(const QString &filePath);
    QString calculateMultiSizeHash(const QStringList &sourceFiles);
    bool isNoNeedConverted(const QString &sourceFile, const QString &currentHash);
    void saveConversionRecord(const QString &sourceFile, const QString &targetFile, const QString &sourceHash);
    void scanAndConvert();
    void cleanupOrphanedDci();

    // 工具函数
    QString getRelativePath(const QString &basePath, const QString &fullPath);
    QStringList getSupportedIconFiles(const QString &directory);

    // 并发转换方法
    void convertMultiSizeIconBatch(const QList<MultiSizeConvertTask> &tasks);
    void convertSingleSizeIconBatch(const QList<ConvertTask> &tasks);
};

HicolorConverter::HicolorConverter()
    : m_totalConverted(0)
    , m_totalSkipped(0)
    , m_totalFailed(0)
{
    // 从环境变量或使用默认值初始化配置

    QString prefix = qEnvironmentVariable("PREFIX", "");

    m_sourceDir = qEnvironmentVariable("SOURCE_DIR", "/usr/share/icons/hicolor");
    m_targetDir = qEnvironmentVariable("TARGET_DIR", prefix + "/usr/share/dsg/icons/convert");
    m_recordFile = qEnvironmentVariable("RECORD_FILE", prefix + "/var/lib/deepin-desktop-theme/dci-conversion-record");
    m_logFile = qEnvironmentVariable("LOG_FILE", prefix + "/var/log/hicolor-dci-converter.log");
    
    // 应用图标目录列表
    m_appDirs << "apps" << "scalable/apps";
    
    // DCI 转换工具路径
    m_dciTool = "/usr/libexec/dtk6/DGui/bin/dci-icon-theme";
    if (!QFile::exists(m_dciTool)) {
        m_dciTool = "/usr/libexec/dtk5/DGui/bin/dci-icon-theme";
    }
}

HicolorConverter::~HicolorConverter()
{
    if (m_logFileHandle.isOpen()) {
        m_logFileHandle.close();
    }
}

bool HicolorConverter::initialize()
{
    // 检查 DCI 工具
    if (!checkDciTool()) {
        return false;
    }
    
    // 创建必要的目录
    if (!createDirectories()) {
        return false;
    }
    
    // 初始化日志文件
    m_logFileHandle.setFileName(m_logFile);
    if (!m_logFileHandle.open(QIODevice::WriteOnly | QIODevice::Append)) {
        qCritical() << "无法打开日志文件:" << m_logFile;
        return false;
    }
    m_logStream.setDevice(&m_logFileHandle);
    
    // 初始化目录扫描缓存
    initializeDirectoryCache();
    
    // 初始化记录缓存
    loadRecordCache();
    
    return true;
}

bool HicolorConverter::checkDciTool()
{
    QFileInfo toolInfo(m_dciTool);
    if (!toolInfo.exists()) {
        qCritical() << "错误: DCI 转换工具不存在:" << m_dciTool;
        qCritical() << "请安装 libdtkgui5-bin 或 libdtkgui6-bin";
        return false;
    }
    
    if (!toolInfo.isExecutable()) {
        qCritical() << "错误: DCI 转换工具不可执行:" << m_dciTool;
        return false;
    }
    
    return true;
}

bool HicolorConverter::createDirectories()
{
    QDir targetDir(m_targetDir);
    if (!targetDir.exists() && !targetDir.mkpath(".")) {
        qCritical() << "无法创建目标目录:" << m_targetDir;
        return false;
    }
    
    QFileInfo recordFileInfo(m_recordFile);
    QDir recordDir = recordFileInfo.dir();
    if (!recordDir.exists() && !recordDir.mkpath(".")) {
        qCritical() << "无法创建记录文件目录:" << recordDir.path();
        return false;
    }
    
    QFileInfo logFileInfo(m_logFile);
    QDir logDir = logFileInfo.dir();
    if (!logDir.exists() && !logDir.mkpath(".")) {
        qCritical() << "无法创建日志文件目录:" << logDir.path();
        return false;
    }
    
    return true;
}

void HicolorConverter::initializeDirectoryCache()
{
    if (m_dirCache.isInitialized) {
        return;
    }
    
    m_dirCache.sizeDirectories.clear();
    m_dirCache.appDirectories.clear();
    m_dirCache.iconFilesByDir.clear();
    m_dirCache.allIconNames.clear();
    
    QDir sourceDir(m_sourceDir);
    QStringList entries = sourceDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    
    // 扫描所有尺寸目录和特殊目录
    for (const QString &entry : entries) {
        for (const QString &context : m_supportedContexts) {
            QString contextDir = m_sourceDir + "/" + entry + "/" + context;
            
            if (QDir(contextDir).exists()) {
                // 检查是否是尺寸目录 (如 16x16, 24x24)
                if (QStringList parts = entry.split('x'); parts.size() == 2 && parts.first() == parts.last()) {
                    if (parts[0].toInt() && parts[1].toInt()) {
                        m_dirCache.sizeDirectories.append(contextDir);
                    }
                } else {
                    // 其他目录 (如 scalable/apps)
                    m_dirCache.appDirectories.append(contextDir);
                }
                
                // 扫描该目录下的图标文件
                QStringList iconFiles = getSupportedIconFiles(contextDir);
                m_dirCache.iconFilesByDir[contextDir] = iconFiles;
                
                // 收集所有图标名称
                for (const QString &iconFile : iconFiles) {
                    QFileInfo fileInfo(iconFile);
                    QString iconName = fileInfo.completeBaseName();
                    m_dirCache.allIconNames.insert(iconName);
                }
            }
        }
    }
    
    m_dirCache.isInitialized = true;
    logMessage(QString("目录扫描缓存初始化完成: %1个尺寸目录, %2个应用目录, %3个图标")
               .arg(m_dirCache.sizeDirectories.size())
               .arg(m_dirCache.appDirectories.size())
               .arg(m_dirCache.allIconNames.size()));
}

void HicolorConverter::logMessage(const QString &message)
{
    QMutexLocker locker(&m_logMutex);
    
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    QString logLine = QString("[%1] %2").arg(timestamp, message);
    
    // 输出到控制台
    qDebug().noquote() << logLine;
    
    // 写入日志文件
    if (m_logStream.device()) {
        m_logStream << logLine << Qt::endl;
        m_logStream.flush();
    }
}

QString HicolorConverter::getFileHash(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(&file);
    return hash.result().toHex();
}

QString HicolorConverter::calculateMultiSizeHash(const QStringList &sourceFiles)
{
    QCryptographicHash hash(QCryptographicHash::Md5);
    
    // 对文件列表排序以确保hash的一致性
    QStringList sortedFiles = sourceFiles;
    sortedFiles.sort();
    
    for (const QString &filePath : sortedFiles) {
        QFile file(filePath);
        if (file.open(QIODevice::ReadOnly)) {
            hash.addData(&file);
            file.close();
        }
    }
    
    return hash.result().toHex();
}

void HicolorConverter::loadRecordCache()
{
    if (m_recordCacheLoaded) {
        return;  // 已经加载过了
    }
    
    QFile recordFile(m_recordFile);
    if (recordFile.open(QIODevice::ReadOnly)) {
        QTextStream stream(&recordFile);
        QString line;
        int recordCount = 0;
        
        while (stream.readLineInto(&line)) {
            QStringList parts = line.split('|');
            if (parts.size() >= 2) {
                QString iconName = parts[0];
                QString hash = parts[1];
                m_recordCache[iconName] = hash;
                recordCount++;
            }
        }
        recordFile.close();
        
        logMessage(QString("加载了 %1 条转换记录到内存").arg(recordCount));
    } else {
        logMessage("转换记录文件不存在或无法读取，使用空缓存");
    }
    
    m_recordCacheLoaded = true;
    m_recordCacheModified = false;
}

void HicolorConverter::flushRecordCache()
{
    if (!m_recordCacheLoaded || !m_recordCacheModified) {
        return;  // 没有加载或没有修改，无需写入
    }
    
    logMessage("将转换记录缓存写入文件");
    
    QMutexLocker locker(&m_recordMutex);
    
    QFile recordFile(m_recordFile);
    if (!recordFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        logMessage(QString("警告: 无法写入转换记录文件: %1").arg(m_recordFile));
        return;
    }
    
    QTextStream stream(&recordFile);
    int recordCount = 0;
    
    for (auto it = m_recordCache.begin(); it != m_recordCache.end(); ++it) {
        stream << it.key() << "|" << it.value() << Qt::endl;
        recordCount++;
    }
    
    recordFile.close();
    
    logMessage(QString("写入了 %1 条转换记录到文件").arg(recordCount));
    m_recordCacheModified = false;
}

bool HicolorConverter::isNoNeedConverted(const QString &sourceFile, const QString &currentHash)
{
    // 确保记录缓存已加载
    if (!m_recordCacheLoaded) {
        loadRecordCache();
    }
    
    QString sourceIconName;

    // 区分多尺寸和单尺寸的情况
    if (sourceFile.startsWith("/")) {
        sourceIconName = QFileInfo(sourceFile).completeBaseName();
    } else {
        sourceIconName = sourceFile;
    }

    // 从内存缓存中查找记录
    if (m_recordCache.contains(sourceIconName)) {
        QString recordedHash = m_recordCache.value(sourceIconName);
        QString targetFile = m_targetDir + "/" + sourceIconName + ".dci";

        if (recordedHash == currentHash && QFile::exists(targetFile)) {
            // 已经转换过，且文件未变化
            return true;
        } else if (recordedHash != currentHash && QFile::exists(targetFile)) {
            // hash不匹配，需要重新转换
            logMessage(QString("=========== %1, %2").arg(recordedHash, currentHash));
            logMessage(QString("源文件已变化，需要重新转换: %1").arg(sourceFile));
            return false;
        }
    } else {
        // 如果转换表中没有记录，检查是否已经存在同名的dci文件
        QString targetFile = m_targetDir + "/" + sourceIconName + ".dci";
        
        if (QFile::exists(targetFile)) {
            // 同名dci文件已经存在，可能是系统已经提供了，不需要再转换
            logMessage(QString("同名dci文件已经存在，可能是系统已经提供了，不需要再转换: %1").arg(sourceFile));
            return true;
        }
    }
    
    return false; 
}

void HicolorConverter::saveConversionRecord(const QString &sourceFile, const QString &targetFile, const QString &sourceHash)
{
    // 确保记录缓存已加载
    if (!m_recordCacheLoaded) {
        loadRecordCache();
    }
    
    // 计算图标名
    QString iconName;
    if (sourceFile.startsWith("/")) {
        // 单尺寸图标：从完整路径提取图标名
        iconName = QFileInfo(sourceFile).completeBaseName();
    } else {
        // 多尺寸图标：sourceFile本身就是图标名
        iconName = sourceFile;
    }

    qWarning() << "saveConversionRecord" << iconName << sourceHash;
    
    // 更新内存缓存
    m_recordCache[iconName] = sourceHash;
    m_recordCacheModified = true;
}

QString HicolorConverter::getRelativePath(const QString &basePath, const QString &fullPath)
{
    QDir baseDir(basePath);
    return baseDir.relativeFilePath(fullPath);
}

QStringList HicolorConverter::getSupportedIconFiles(const QString &directory)
{
    QStringList result;
    QDir dir(directory);
    
    if (!dir.exists()) {
        return result;
    }
    
    QStringList filters;
    filters << "*.svg" << "*.png";
    
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
    for (const QFileInfo &fileInfo : files) {
        result << fileInfo.absoluteFilePath();
    }
    
    return result;
}

void HicolorConverter::scanAndConvert()
{
    QSet<QString> processedIcons;  // 已处理的图标名称
    
    // 收集多尺寸图标任务
    QMap<QString, MultiSizeConvertTask> multiSizeTasks;

    // 所有图标的所有尺寸文件
    QMap<QString, QStringList> allIconSizes;
    
    for (const QString &sizeDir : m_dirCache.sizeDirectories) {
        // 获取该目录下的所有支持的图标文件
        QStringList iconFiles = m_dirCache.iconFilesByDir.value(sizeDir);
        
        // 从路径中提取尺寸信息 (如 16x16/apps -> 16)
        QFileInfo dirInfo(sizeDir);
        QString sizeStr = dirInfo.dir().dirName();
        if (sizeStr.contains("x")) {
            sizeStr = sizeStr.split("x").first();
        }
        
        for (const QString &sourceFile : iconFiles) {
            QFileInfo sourceInfo(sourceFile);
            QString iconName = sourceInfo.completeBaseName();
            
            // 收集所有尺寸的文件
            if (!allIconSizes.contains(iconName)) {
                allIconSizes[iconName] = QStringList();
            }
            allIconSizes[iconName].append(sourceFile);
            
            // 同时记录尺寸信息
            if (!multiSizeTasks.contains(iconName)) {
                multiSizeTasks[iconName] = MultiSizeConvertTask{iconName, {}, {}};
            }
            multiSizeTasks[iconName].sourceFiles.append(sourceFile);
            multiSizeTasks[iconName].sizes.append(sizeStr);
        }
    }
    
    // 判断这些图标是否需要转换
    QMap<QString, MultiSizeConvertTask> finalMultiSizeTasks;
    
    for (auto it = multiSizeTasks.begin(); it != multiSizeTasks.end(); ++it) {
        QString iconName = it.key();
        MultiSizeConvertTask &task = it.value();
        
        // 如果该图标已经被处理过，跳过
        if (processedIcons.contains(iconName)) {
            continue;
        }

        // 计算所有尺寸文件的综合hash
        QString combinedHash = calculateMultiSizeHash(task.sourceFiles);

        if (!isNoNeedConverted(iconName, combinedHash)) {
            finalMultiSizeTasks[iconName] = task;
            logMessage(QString("多尺寸图标 %1 需要转换，包含 %2 个尺寸").arg(iconName).arg(task.sourceFiles.size()));
        } else {
            processedIcons.insert(iconName);
            // logMessage(QString("多尺寸图标 %1 无需转换").arg(iconName));
        }
    }
    
    // 多尺寸图标开始转换
    if (!finalMultiSizeTasks.isEmpty()) {
        QList<MultiSizeConvertTask> tasks = finalMultiSizeTasks.values();
        logMessage(QString("准备批量转换 %1 个多尺寸图标").arg(tasks.size()));
        convertMultiSizeIconBatch(tasks);
        
        // 标记这些图标已处理
        for (const auto &task : tasks) {
            processedIcons.insert(task.iconName);
        }
    } else {
        logMessage("没有需要转换的多尺寸图标");
    }
    
    QList<ConvertTask> singleSizeIconTasks;
    
    // 按优先级顺序处理图标
    for (const QString &priority : m_iconPriorities) {
        QString priorityDir = m_sourceDir + "/" + priority;
        
        // 检查该优先级目录是否存在于缓存中
        if (!m_dirCache.iconFilesByDir.contains(priorityDir)) {
            continue;
        }
        
        // 获取该目录下的所有支持的图标文件
        QStringList iconFiles = m_dirCache.iconFilesByDir.value(priorityDir);
        
        for (const QString &sourceFile : iconFiles) {
            QFileInfo sourceInfo(sourceFile);
            QString iconName = sourceInfo.completeBaseName();
            
            // 如果该图标已经被处理过，跳过
            if (processedIcons.contains(iconName)) {
                continue;
            }
            
            // 检查是否需要转换
            QString sourceHash = getFileHash(sourceFile);
            if (!isNoNeedConverted(sourceFile, sourceHash)) {
                QString relativePath = getRelativePath(m_sourceDir, sourceFile);
                singleSizeIconTasks.append({sourceFile, relativePath});
                processedIcons.insert(iconName);  // 标记为已处理
            }
        }
    }
    
    // 执行单尺寸图标批量转换
    if (!singleSizeIconTasks.isEmpty()) {
        logMessage(QString("准备批量转换 %1 个单尺寸图标").arg(singleSizeIconTasks.size()));
        convertSingleSizeIconBatch(singleSizeIconTasks);
    } else {
        logMessage("没有需要转换的单尺寸图标，跳过");
    }
}

void HicolorConverter::cleanupOrphanedDci()
{
    logMessage("开始清理孤立的 dci 文件");
    
    // 确保记录缓存已加载
    if (!m_recordCacheLoaded) {
        loadRecordCache();
    }
    
    int cleanedCount = 0;
    
    // 遍历内存缓存中的记录
    QStringList toRemove;
    for (auto it = m_recordCache.begin(); it != m_recordCache.end(); ++it) {
        QString iconName = it.key();
        
        // 检查该图标是否还存在于原始目录中
        bool shouldKeep = m_dirCache.allIconNames.contains(iconName);
        
        if (!shouldKeep) {
            // 源文件不存在，删除对应的 dci 文件
            QString targetFile = m_targetDir + "/" + iconName + ".dci";
            if (QFile::exists(targetFile)) {
                if (QFile::remove(targetFile)) {
                    logMessage(QString("删除孤立的 dci 文件: %1 (源图标已删除: %2)").arg(targetFile, iconName));
                    cleanedCount++;
                }
            }
            // 标记从缓存中移除
            toRemove << iconName;
        }
    }
    
    // 从内存缓存中移除孤立的记录
    for (const QString &iconName : toRemove) {
        m_recordCache.remove(iconName);
        m_recordCacheModified = true;
    }
    
    logMessage(QString("清理完成 - 删除孤立文件: %1").arg(cleanedCount));
}

void HicolorConverter::convertMultiSizeIconBatch(const QList<MultiSizeConvertTask> &tasks)
{
    if (tasks.isEmpty()) {
        return;
    }
    
    QString tempDir = QDir::temp().absoluteFilePath("hicolor-convert-multisize");
    QDir().mkpath(tempDir);

    QSet<QString> createdSizeDirs;
    
    // 准备目录结构 16/ 24/ 32/ 48/ 64/ 96/ 128/ 256/
    for (const MultiSizeConvertTask &task : tasks) {
        for (int i = 0; i < task.sourceFiles.size(); ++i) {
            QString sourceFile = task.sourceFiles[i];
            QString size = task.sizes[i];
            
            // 创建尺寸目录 (如 16/, 24/)
            QString sizeDir = tempDir + "/" + size;
            if (!createdSizeDirs.contains(sizeDir)) {
                QDir().mkpath(sizeDir);
                createdSizeDirs.insert(sizeDir);
            }
            
            // 复制文件到对应尺寸目录
            QFileInfo sourceInfo(sourceFile);
            QString destFile = sizeDir + "/" + sourceInfo.fileName();
            QFile::copy(sourceFile, destFile);
        }
    }
    
    // 准备输出目录
    QString multiSizeOutputDir = m_targetDir + "_multisize_temp";
    
    // 确保输出目录不存在
    if (QDir(multiSizeOutputDir).exists()) {
        QDir(multiSizeOutputDir).removeRecursively();
    }

    QStringList arguments;
    arguments << tempDir;
    arguments << "-o" << multiSizeOutputDir;
    arguments << "-O" << "3=95";
    
    QProcess process;
    process.start(m_dciTool, arguments);
    process.waitForFinished(-1);

    if (process.exitCode() == 0) {
        logMessage(QString("多尺寸图标批量转换成功，共转换 %1 个图标").arg(tasks.size()));
        
        // 将生成的dci文件移动到目标目录
        QDir outputDir(multiSizeOutputDir);
        QStringList dciFiles = outputDir.entryList(QStringList() << "*.dci", QDir::Files);
        
        for (const QString &dciFile : dciFiles) {
            QString sourcePath = multiSizeOutputDir + "/" + dciFile;
            QString targetPath = m_targetDir + "/" + dciFile;
            
            if (QFile::exists(targetPath)) {
                QFile::remove(targetPath);
            }
            QFile::copy(sourcePath, targetPath);
        }
        
        // 记录转换结果
        for (const MultiSizeConvertTask &task : tasks) {
            QString combinedHash = calculateMultiSizeHash(task.sourceFiles);
            QString targetFile = m_targetDir + "/" + task.iconName + ".dci";
            saveConversionRecord(task.iconName, targetFile, combinedHash);
            m_totalConverted++;
        }
        
        // 清理临时输出目录
        QDir(multiSizeOutputDir).removeRecursively();
    } else {
        logMessage(QString("多尺寸图标批量转换失败: %1").arg(QString::fromLocal8Bit(process.readAllStandardError())));
        m_totalFailed += tasks.size();
    }
    
    // 清理临时目录
    QDir(tempDir).removeRecursively();
}

void HicolorConverter::convertSingleSizeIconBatch(const QList<ConvertTask> &tasks)
{
    if (tasks.isEmpty()) {
        return;
    }
    
    // 创建单尺寸图标的临时目录
    QString singleSizeTempDir = QDir::temp().absoluteFilePath("hicolor-convert-singlesize");
    QDir().mkpath(singleSizeTempDir);
    
    // 准备单尺寸图标的目录结构
    // 创建scalable目录（单尺寸图标统一放在scalable下）
    QString scalableDir = singleSizeTempDir + "/scalable";
    QDir().mkpath(scalableDir);
    
    // 复制所有单尺寸图标文件到scalable目录
    for (const ConvertTask &task : tasks) {
        QFileInfo sourceInfo(task.sourceFile);
        QString destFile = scalableDir + "/" + sourceInfo.fileName();
        QFile::copy(task.sourceFile, destFile);
    }
    
    // 为单尺寸图标转换准备输出目录
    QString singleSizeOutputDir = m_targetDir + "_singlesize_temp";
    
    // 确保输出目录不存在
    if (QDir(singleSizeOutputDir).exists()) {
        QDir(singleSizeOutputDir).removeRecursively();
    }
    
    // 执行单尺寸图标批量转换命令
    QStringList arguments;
    arguments << singleSizeTempDir;
    arguments << "-o" << singleSizeOutputDir;
    arguments << "-O" << "3=95";
    
    QProcess process;
    process.start(m_dciTool, arguments);
    process.waitForFinished(-1);
    
    if (process.exitCode() == 0) {
        logMessage(QString("单尺寸图标批量转换成功，共转换 %1 个图标").arg(tasks.size()));
        
        // 将生成的dci文件移动到目标目录
        QDir outputDir(singleSizeOutputDir);
        QStringList dciFiles = outputDir.entryList(QStringList() << "*.dci", QDir::Files);
        
        for (const QString &dciFile : dciFiles) {
            QString sourcePath = singleSizeOutputDir + "/" + dciFile;
            QString targetPath = m_targetDir + "/" + dciFile;
            
            if (QFile::exists(targetPath)) {
                QFile::remove(targetPath);
            }
            QFile::copy(sourcePath, targetPath);
        }
        
        // 记录转换结果
        for (const ConvertTask &task : tasks) {
            QFileInfo sourceInfo(task.sourceFile);
            QString iconName = sourceInfo.completeBaseName();
            QString sourceHash = getFileHash(task.sourceFile);
            QString targetFile = m_targetDir + "/" + iconName + ".dci";
            saveConversionRecord(task.sourceFile, targetFile, sourceHash);
            m_totalConverted++;
        }
        
        // 清理临时输出目录
        QDir(singleSizeOutputDir).removeRecursively();
    } else {
        logMessage(QString("单尺寸图标批量转换失败: %1").arg(QString::fromLocal8Bit(process.readAllStandardError())));
        m_totalFailed += tasks.size();
    }
    
    // 清理临时目录
    QDir(singleSizeTempDir).removeRecursively();
}

int HicolorConverter::run()
{
    logMessage("==== 开始 ====");
    // 扫描并转换图标
    scanAndConvert();
    
    // 清理孤立的 dci 文件
    cleanupOrphanedDci();
    
    // 将内存缓存写入文件
    flushRecordCache();
    logMessage("==== 完成 ====");

    return 0;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("hicolorconvert");
    app.setApplicationVersion("1.0");
    
    QCommandLineParser parser;
    parser.setApplicationDescription("hicolor 应用图标到 dci 格式转换工具");
    parser.addHelpOption();
    parser.addVersionOption();
    
    // 添加命令行选项
    QCommandLineOption sourceOption(QStringList() << "s" << "source", 
                                   "源目录路径", "path", "/usr/share/icons/hicolor");
    QCommandLineOption targetOption(QStringList() << "t" << "target", 
                                   "目标目录路径", "path", "/usr/share/icons/convert");
    QCommandLineOption recordOption(QStringList() << "r" << "record", 
                                   "记录文件路径", "path", "/var/lib/deepin-desktop-theme/dci-conversion-record");
    QCommandLineOption logOption(QStringList() << "l" << "log", 
                                "日志文件路径", "path", "/var/log/hicolor-dci-converter.log");
    
    parser.addOption(sourceOption);
    parser.addOption(targetOption);
    parser.addOption(recordOption);
    parser.addOption(logOption);
    
    parser.process(app);
    
    // 设置环境变量（如果命令行参数提供了的话）
    if (parser.isSet(sourceOption)) {
        qputenv("SOURCE_DIR", parser.value(sourceOption).toUtf8());
    }
    if (parser.isSet(targetOption)) {
        qputenv("TARGET_DIR", parser.value(targetOption).toUtf8());
    }
    if (parser.isSet(recordOption)) {
        qputenv("RECORD_FILE", parser.value(recordOption).toUtf8());
    }
    if (parser.isSet(logOption)) {
        qputenv("LOG_FILE", parser.value(logOption).toUtf8());
    }
    
    HicolorConverter converter;
    
    if (!converter.initialize()) {
        return 1;
    }
    
    return converter.run();
}
