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

// 转换结果结构
struct ConvertResult {
    bool success;
    QString sourceFile;
    QString targetFile;
    QString errorMessage;
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
    
    void logMessage(const QString &message);
    QString getFileHash(const QString &filePath);
    QString calculateMultiSizeHash(const QStringList &sourceFiles);
    bool isNoNeedConverted(const QString &sourceFile, const QString &currentHash);
    void saveConversionRecord(const QString &sourceFile, const QString &targetFile, const QString &sourceHash);
    bool convertIconFile(const QString &sourceFile);
    void scanAndConvert();
    void cleanupOrphanedDci();
    
    // 工具函数
    QString getRelativePath(const QString &basePath, const QString &fullPath);
    QStringList getSupportedIconFiles(const QString &directory);
    QList<MultiSizeConvertTask> collectMultiSizeIcons();
    
    // 并发转换方法
    ConvertResult convertIconConcurrent(const ConvertTask &task);
    ConvertResult convertMultiSizeIconConcurrent(const MultiSizeConvertTask &task);
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
    
    logMessage("初始化目录扫描缓存...");
    
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
                if (entry.contains('x') && entry.split('x').size() == 2) {
                    QStringList parts = entry.split('x');
                    bool ok1, ok2;
                    parts[0].toInt(&ok1);
                    parts[1].toInt(&ok2);
                    if (ok1 && ok2) {
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

bool HicolorConverter::isNoNeedConverted(const QString &sourceFile, const QString &currentHash)
{
    logMessage(QString("检查是否需要转换: %1, %2").arg(sourceFile).arg(currentHash));
    bool isRecord = false;
    
    QString sourceIconName;

    // 区分传递绝对路径和图标名的情况
    if (sourceFile.startsWith("/")) {
        sourceIconName = QFileInfo(sourceFile).completeBaseName();
    } else {
        sourceIconName = sourceFile;
    }

    logMessage(QString("sourceIconName: %1").arg(sourceIconName));
    QFile recordFile(m_recordFile);
    if (recordFile.open(QIODevice::ReadOnly)) {
        QTextStream stream(&recordFile);
        QString line;
        
        while (stream.readLineInto(&line)) {
            QStringList parts = line.split('|');
            if (parts.size() >= 2) {
                QString recordedIconName = parts[0];
                QString recordedHash = parts[1];

                // 如果有转换记录
                if (recordedIconName == sourceIconName) {
                    isRecord = true;
                    
                    // 构造目标文件路径
                    QString targetFile = m_targetDir + "/" + recordedIconName + ".dci";

                    if (recordedHash == currentHash && QFile::exists(targetFile)) {
                        // 已经转换过，且文件未变化
                        logMessage(QString("跳过已转换: %1").arg(sourceFile));
                        return true;
                    } else if (recordedHash != currentHash && QFile::exists(targetFile)) {
                        // hash不匹配，需要重新转换
                        logMessage(QString("源文件已变化，需要重新转换: %1").arg(sourceFile));
                        return false;
                    }

                    break;
                }
            }
        }
    }

    // 如果转换表中没有记录，检查是否已经存在同名的dci文件
    if (!isRecord) {
        QFileInfo sourceInfo(sourceFile);
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
    QMutexLocker locker(&m_recordMutex);
    
    QFile recordFile(m_recordFile);
    if (!recordFile.open(QIODevice::WriteOnly | QIODevice::Append)) {
        logMessage(QString("警告: 无法写入转换记录文件: %1").arg(m_recordFile));
        return;
    }
    
    QTextStream stream(&recordFile);
    
    // 计算图标名
    QString iconName;
    if (sourceFile.startsWith("/")) {
        // 单尺寸图标：从完整路径提取图标名
        iconName = QFileInfo(sourceFile).completeBaseName();
    } else {
        // 多尺寸图标：sourceFile本身就是图标名
        iconName = sourceFile;
    }
    
    // 简化格式：只存储图标名和hash
    stream << iconName << "|" << sourceHash << Qt::endl;
}

bool HicolorConverter::convertIconFile(const QString &sourceFile)
{
    QFileInfo sourceInfo(sourceFile);
    QString targetFile = m_targetDir + "/" + sourceInfo.completeBaseName() + ".dci";
    
    // 检查是否需要转换
    QString currentHash = getFileHash(sourceFile);
    if (isNoNeedConverted(sourceFile, currentHash)) {
        // 文件未变化，跳过转换
        return true;
    }
    
    // 创建临时目录
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        logMessage(QString("转换失败: 无法创建临时目录: %1").arg(sourceFile));
        return false;
    }
    
    // 创建临时源目录，直接放置单个图标文件
    QString tempSourceDir = tempDir.path() + "/source";
    QString tempTargetDir = tempDir.path() + "/target";
    
    QDir().mkpath(tempSourceDir);
    
    // 复制源文件到临时目录，使用原始文件名
    QString tempSourceFile = tempSourceDir + "/" + sourceInfo.fileName();
    if (!QFile::copy(sourceFile, tempSourceFile)) {
        logMessage(QString("转换失败: 无法复制源文件到临时目录: %1").arg(sourceFile));
        return false;
    }
    
    // 执行转换
    QProcess process;
    process.setWorkingDirectory(tempDir.path());
    
    // 使用 env 命令来确保环境变量生效
    process.setProgram(m_dciTool);
    process.setArguments({tempSourceDir, "-o", tempTargetDir, "-O", "3=95"});
    
    process.start();
    
    if (!process.waitForFinished(30000)) { // 30秒超时
        logMessage(QString("转换失败: dci-icon-theme 超时: %1").arg(sourceFile));
        process.kill();
        return false;
    }
    
    QString standardOutput = process.readAllStandardOutput();
    QString errorOutput = process.readAllStandardError();
    
    if (!errorOutput.isEmpty()) {
        logMessage(QString("错误输出: %1").arg(errorOutput));
    }
    
    if (process.exitCode() != 0) {
        logMessage(QString("转换失败: dci-icon-theme 执行出错: %1, 退出代码: %2").arg(sourceFile).arg(process.exitCode()));
        return false;
    }
    
    // 查找生成的 dci 文件（现在应该直接在目标目录根目录）
    QString expectedDciFile = tempTargetDir + "/" + sourceInfo.completeBaseName() + ".dci";
    
    if (QFile::exists(expectedDciFile)) {
        // 移动到目标位置
        if (QFile::exists(targetFile)) {
            QFile::remove(targetFile);
        }
        
        if (QFile::copy(expectedDciFile, targetFile)) {
            logMessage(QString("转换成功: %1 -> %2").arg(sourceFile, targetFile));
            
            // 记录转换信息
            QString sourceHash = getFileHash(sourceFile);
            saveConversionRecord(sourceFile, targetFile, sourceHash);
            
            return true;
        } else {
            logMessage(QString("转换失败: 无法移动生成的 dci 文件: %1").arg(sourceFile));
        }
    } else {
        // 如果直接查找失败，回退到递归查找
        QDirIterator it(tempTargetDir, QStringList() << "*.dci", QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) {
            QString generatedDci = it.next();
            
            // 移动到目标位置
            if (QFile::exists(targetFile)) {
                QFile::remove(targetFile);
            }
            
            if (QFile::copy(generatedDci, targetFile)) {
                logMessage(QString("转换成功: %1 -> %2").arg(sourceFile, targetFile));
                
                // 记录转换信息
                QString sourceHash = getFileHash(sourceFile);
                saveConversionRecord(sourceFile, targetFile, sourceHash);
                
                return true;
            } else {
                logMessage(QString("转换失败: 无法移动生成的 dci 文件: %1").arg(sourceFile));
            }
        } else {
            logMessage(QString("转换失败: 未找到生成的 dci 文件: %1").arg(sourceFile));
        }
    }
    
    return false;
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

ConvertResult HicolorConverter::convertIconConcurrent(const ConvertTask &task)
{
    ConvertResult result;
    result.success = false;
    result.sourceFile = task.sourceFile;
    
    // 检查是否已经转换过
    QString currentHash = getFileHash(task.sourceFile);
    if (isNoNeedConverted(task.sourceFile, currentHash)) {
        // logMessage(QString("跳过已转换: %1").arg(task.sourceFile));
        result.success = true; // 跳过也算成功
        result.targetFile = ""; // 空字符串表示跳过
        return result;
    }
    
    // 计算目标文件路径 - 直接放在目标目录下
    QFileInfo sourceInfo(task.sourceFile);
    QString targetFile = m_targetDir + "/" + sourceInfo.completeBaseName() + ".dci";
    result.targetFile = targetFile;
    
    // 确保目标目录存在（应该在初始化时已经创建）
    QDir targetDir(m_targetDir);
    if (!targetDir.exists()) {
        result.errorMessage = QString("目标目录不存在: %1").arg(m_targetDir);
        return result;
    }
    
    // 创建临时目录用于转换
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        result.errorMessage = QString("无法创建临时目录");
        return result;
    }
    
    // 创建临时源目录，直接放置单个图标文件
    QString tempSourceDir = tempDir.path() + "/source";
    QString tempTargetDir = tempDir.path() + "/target";
    
    QDir().mkpath(tempSourceDir);
    
    // 复制源文件到临时目录，使用原始文件名
    QString tempSourceFile = tempSourceDir + "/" + sourceInfo.fileName();
    if (!QFile::copy(task.sourceFile, tempSourceFile)) {
        result.errorMessage = QString("无法复制源文件到临时目录");
        return result;
    }
    
    // 执行转换
    QProcess process;
    process.setWorkingDirectory(tempDir.path());
    
    process.setProgram(m_dciTool);
    process.setArguments({tempSourceDir, "-o", tempTargetDir, "-O", "3=95"});
    
    process.start();
    
    if (!process.waitForFinished(30000)) { // 30秒超时
        result.errorMessage = QString("dci-icon-theme 超时") + process.readAll();
        process.kill();
        return result;
    }
    
    if (process.exitCode() != 0) {
        result.errorMessage = QString("dci-icon-theme 执行出错，退出代码: %1").arg(process.exitCode());
        return result;
    }
    
    // 查找生成的 dci 文件（现在应该直接在目标目录根目录）
    QString expectedDciFile = tempTargetDir + "/" + sourceInfo.completeBaseName() + ".dci";
    if (QFile::exists(expectedDciFile)) {
        // 移动到目标位置 TODO 如果有就不要删除了，可能我们已经有自己画好的图标，只添加没有的
        if (QFile::exists(targetFile)) {
            QFile::remove(targetFile);
        }
        
        if (QFile::copy(expectedDciFile, targetFile)) {
            // 记录转换信息
            saveConversionRecord(task.sourceFile, targetFile, currentHash);
            result.success = true;
            return result;
        } else {
            result.errorMessage = QString("无法移动生成的 dci 文件") + expectedDciFile + " " + targetFile + (QChar)QFile::exists(targetFile);
        }
    } else {
        // 如果直接查找失败，回退到递归查找
        QDirIterator it(tempTargetDir, QStringList() << "*.dci", QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) {
            QString generatedDci = it.next();
            // 移动到目标位置
            if (QFile::exists(targetFile)) {
                QFile::remove(targetFile);
            }
            
            if (QFile::copy(generatedDci, targetFile)) {
                // 记录转换信息
                saveConversionRecord(task.sourceFile, targetFile, currentHash);
                result.success = true;
                return result;
            } else {
                result.errorMessage = QString("无法移动生成的 dci 文件") + generatedDci + " " + targetFile + (QChar)QFile::exists(targetFile);
            }
        } else {
            result.errorMessage = QString("未找到生成的 dci 文件");
        }
    }
    
    return result;
}

QList<MultiSizeConvertTask> HicolorConverter::collectMultiSizeIcons()
{
    QList<MultiSizeConvertTask> tasks;
    QMap<QString, MultiSizeConvertTask> iconGroups;
    
    // 使用缓存的尺寸目录信息
    for (const QString &sizeDir : m_dirCache.sizeDirectories) {
        QString size = QFileInfo(sizeDir).dir().dirName(); // 获取尺寸 (如 "16x16")
        QString sizeValue = size.split('x').first(); // 获取尺寸值 (如 "16")
        
        QStringList iconFiles = m_dirCache.iconFilesByDir.value(sizeDir);
        
        for (const QString &iconFile : iconFiles) {
            QFileInfo fileInfo(iconFile);
            QString iconName = fileInfo.completeBaseName();

            if (!iconGroups.contains(iconName)) {
                iconGroups[iconName].iconName = iconName;
            }

            iconGroups[iconName].sourceFiles.append(iconFile);
            iconGroups[iconName].sizes.append(sizeValue);
        }
    }
    
    // 过滤出有多个尺寸的图标，并检查是否需要转换
    for (auto it = iconGroups.begin(); it != iconGroups.end(); ++it) {
        if (it.value().sourceFiles.size() >= 1) {
            // 计算多尺寸图标的综合hash
            QString combinedHash = calculateMultiSizeHash(it.value().sourceFiles);
            
            // 检查是否需要转换
            if (!isNoNeedConverted(it.value().iconName, combinedHash)) {
                tasks.append(it.value());
            }
        }
    }
    
    // logMessage(QString("收集到 %1 个多尺寸图标").arg(tasks.size()));
    return tasks;
}

ConvertResult HicolorConverter::convertMultiSizeIconConcurrent(const MultiSizeConvertTask &task)
{
    ConvertResult result;
    result.success = false;
    result.sourceFile = task.iconName;
    
    QString targetFile = m_targetDir + "/" + task.iconName + ".dci";
    result.targetFile = targetFile;
    
    // 检查是否需要转换（检查所有源文件的哈希）
    QString combinedHash;
    for (const QString &sourceFile : task.sourceFiles) {
        combinedHash += getFileHash(sourceFile);
    }
    
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(combinedHash.toUtf8());
    QString finalHash = hash.result().toHex();
    
    if (isNoNeedConverted(task.iconName, finalHash)) {
        result.success = true;
        result.targetFile = ""; // 空字符串表示跳过
        return result;
    }
    
    // 创建临时目录
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        result.errorMessage = QString("无法创建临时目录");
        return result;
    }
    
    QString tempSourceDir = tempDir.path() + "/source";
    QString tempTargetDir = tempDir.path() + "/target";
    
    // 创建扁平的尺寸目录结构
    for (int i = 0; i < task.sourceFiles.size(); ++i) {
        QString sizeDir = tempSourceDir + "/" + task.sizes[i];
        QDir().mkpath(sizeDir);
        
        QFileInfo sourceInfo(task.sourceFiles[i]);
        QString tempFile = sizeDir + "/" + task.iconName + "." + sourceInfo.suffix();
        
        if (!QFile::copy(task.sourceFiles[i], tempFile)) {
            result.errorMessage = QString("无法复制源文件: %1").arg(task.sourceFiles[i]);
            return result;
        }
    }
    
    QProcess process;
    process.setWorkingDirectory(tempDir.path());
    process.setProgram(m_dciTool);
    
    process.setArguments({tempSourceDir, "-o", tempTargetDir, "-O", "3=95"});
    
    process.start();
    
    if (!process.waitForFinished(30000)) {
        result.errorMessage = QString("dci-icon-theme 超时").append(process.readAllStandardError());
        process.kill();
        return result;
    }
    
    if (process.exitCode() != 0) {
        result.errorMessage = QString("dci-icon-theme 执行出错，退出代码: %1").arg(process.exitCode());
        return result;
    }
    
    // 查找生成的 dci 文件
    QString expectedDciFile = tempTargetDir + "/" + task.iconName + ".dci";
    
    if (QFile::exists(expectedDciFile)) {
        // 移动到目标位置
        if (QFile::exists(targetFile)) {
            QFile::remove(targetFile);
        }
        
        if (QFile::copy(expectedDciFile, targetFile)) {
            // 记录转换信息
            saveConversionRecord(task.iconName, targetFile, finalHash);
            result.success = true;
            return result;
        } else {
            result.errorMessage = QString("无法移动生成的 dci 文件");
        }
    } else {
        // 查找实际生成的 dci 文件（可能有不同的命名）
        QDirIterator it(tempTargetDir, QStringList() << "*.dci", QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) {
            QString generatedDci = it.next();
            
            // 移动到目标位置
            if (QFile::exists(targetFile)) {
                QFile::remove(targetFile);
            }
            
            if (QFile::copy(generatedDci, targetFile)) {
                // 记录转换信息
                saveConversionRecord(task.iconName, targetFile, finalHash);
                result.success = true;
                return result;
            } else {
                result.errorMessage = QString("无法移动生成的 dci 文件") + generatedDci + " " + targetFile;
            }
        } else {
            result.errorMessage = QString("未找到生成的 dci 文件");
        }
    }
    
    return result;
}

void HicolorConverter::scanAndConvert()
{
    logMessage("==== 开始分阶段转换 hicolor 应用图标 ====");
    
    // 阶段1: 扫描和准备阶段
    logMessage("阶段1: 扫描所有图标并准备转换任务");
    
    QSet<QString> processedIcons;  // 已处理的图标名称
    
    // 优先处理多尺寸图标（*x*/apps 尺寸图标）- 最高优先级
    logMessage("优先处理多尺寸图标（按尺寸目录分组批量转换）");
    
    // 收集多尺寸图标任务
    QMap<QString, MultiSizeConvertTask> multiSizeTasks;  // iconName -> MultiSizeConvertTask
    
    for (const QString &sizeDir : m_dirCache.sizeDirectories) {
        logMessage(QString("扫描尺寸目录: %1").arg(sizeDir));
        
        // 获取该目录下的所有支持的图标文件
        QStringList iconFiles = m_dirCache.iconFilesByDir.value(sizeDir);
        
        // 从路径中提取尺寸信息 (如 16x16/apps -> 16)
        QFileInfo dirInfo(sizeDir);
        QString sizeStr = dirInfo.dir().dirName();
        if (sizeStr.contains("x")) {
            sizeStr = sizeStr.split("x").first();  // 16x16 -> 16
        }
        
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
                // 添加到多尺寸任务中
                if (!multiSizeTasks.contains(iconName)) {
                    multiSizeTasks[iconName] = MultiSizeConvertTask{iconName, {}, {}};
                }
                multiSizeTasks[iconName].sourceFiles.append(sourceFile);
                multiSizeTasks[iconName].sizes.append(sizeStr);
            }
        }
    }
    
    // 执行多尺寸图标批量转换
    if (!multiSizeTasks.isEmpty()) {
        QList<MultiSizeConvertTask> tasks = multiSizeTasks.values();
        logMessage(QString("准备批量转换 %1 个多尺寸图标").arg(tasks.size()));
        convertMultiSizeIconBatch(tasks);
        
        // 标记这些图标已处理
        for (const auto &task : tasks) {
            processedIcons.insert(task.iconName);
        }
    }
    
    // 阶段2: 处理其他优先级的单尺寸图标
    logMessage("阶段2: 处理其他优先级的单尺寸图标");
    
    QList<ConvertTask> singleSizeIconTasks;
    
    // 按优先级顺序处理图标
    for (const QString &priority : m_iconPriorities) {
        QString priorityDir = m_sourceDir + "/" + priority;
        
        // 检查该优先级目录是否存在于缓存中
        if (!m_dirCache.iconFilesByDir.contains(priorityDir)) {
            continue;
        }
        
        logMessage(QString("扫描优先级目录: %1").arg(priorityDir));
        
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
    
    QFile recordFile(m_recordFile);
    if (!recordFile.open(QIODevice::ReadOnly)) {
        logMessage("没有转换记录文件，跳过清理");
        return;
    }
    
    QStringList validRecords;
    int cleanedCount = 0;
    
    QTextStream stream(&recordFile);
    QString line;
    
    while (stream.readLineInto(&line)) {
        QStringList parts = line.split('|');
        if (parts.size() >= 2) {
            QString iconName = parts[0];
            QString recordedHash = parts[1];
            
            // 检查该图标是否还存在于原始目录中
            bool shouldKeep = m_dirCache.allIconNames.contains(iconName);
            
            if (shouldKeep) {
                // 源文件存在，保留记录
                validRecords << line;
            } else {
                // 源文件不存在，删除对应的 dci 文件
                QString targetFile = m_targetDir + "/" + iconName + ".dci";
                if (QFile::exists(targetFile)) {
                    if (QFile::remove(targetFile)) {
                        logMessage(QString("删除孤立的 dci 文件: %1 (源图标已删除: %2)").arg(targetFile, iconName));
                        cleanedCount++;
                    }
                }
            }
        }
    }
    
    recordFile.close();
    
    // 重写记录文件
    if (recordFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream writeStream(&recordFile);
        for (const QString &record : validRecords) {
            writeStream << record << "\n";
        }
        recordFile.close();
    }
    
    logMessage(QString("清理完成 - 删除孤立文件: %1").arg(cleanedCount));
}

void HicolorConverter::convertMultiSizeIconBatch(const QList<MultiSizeConvertTask> &tasks)
{
    if (tasks.isEmpty()) {
        return;
    }
    
    logMessage(QString("阶段2: 开始批量转换 %1 个多尺寸图标").arg(tasks.size()));
    
    // 创建多尺寸图标的临时目录
    QString tempDir = QDir::temp().absoluteFilePath("hicolor-convert-multisize");
    QDir().mkpath(tempDir);
    
    // 准备多尺寸图标的目录结构
    // 按尺寸组织目录结构，将所有多尺寸图标按尺寸分类放置
    QSet<QString> createdSizeDirs;
    
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
    
    // 为多尺寸图标转换准备输出目录
    QString multiSizeOutputDir = m_targetDir + "_multisize_temp";
    
    // 确保输出目录不存在
    if (QDir(multiSizeOutputDir).exists()) {
        QDir(multiSizeOutputDir).removeRecursively();
    }
    
    // 批量执行转换命令
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
    
    logMessage(QString("阶段3: 开始批量转换 %1 个单尺寸图标").arg(tasks.size()));
    
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
    logMessage("==== hicolor 应用图标到 dci 转换器启动 ====");
    
    // 扫描并转换图标
    scanAndConvert();
    
    // 清理孤立的 dci 文件
    cleanupOrphanedDci();
    
    logMessage("==== hicolor 应用图标到 dci 转换器完成 ====");
    
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
