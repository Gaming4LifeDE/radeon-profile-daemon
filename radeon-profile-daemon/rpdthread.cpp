
// copyright marazmista @ 12.05.2014

#include "rpdthread.h"
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QStringList>
#include <QProcess>
#include <QDataStream>

rpdThread::rpdThread() : QThread(),
    signalReceiver(nullptr)
{
    qDebug() << "Starting in debug mode";

    QLocalServer::removeServer(serverName);

    daemonServer.listen(serverName);
    connect(&daemonServer,SIGNAL(newConnection()),this,SLOT(newConn()));
    QFile::setPermissions("/tmp/"+serverName,QFile("/tmp/"+serverName).permissions() | QFile::WriteOther | QFile::ReadOther);

    qDebug() << "ok";

    connect(&timer,SIGNAL(timeout()),this,SLOT(onTimer()));
}

void rpdThread::newConn() {
    qWarning() << "Connecting to the client";
    signalReceiver = daemonServer.nextPendingConnection();
    connect(signalReceiver,SIGNAL(readyRead()),this,SLOT(decodeSignal()));
    connect(signalReceiver,SIGNAL(disconnected()),this,SLOT(disconnected()));
}

void rpdThread::disconnected() {
    qWarning() << "Disconnecting from the client";
    timer.stop();

    if (sharedMem.isAttached())
        sharedMem.detach();

    signalReceiver->deleteLater();
}


void rpdThread::decodeSignal() {
    char signal[256] = {0};

    signalReceiver->read(signal,signalReceiver->bytesAvailable());
    performTask(QString(signal));
}

void rpdThread::onTimer() {
    if (signalReceiver->state() == QLocalSocket::ConnectedState)
        readData();
}

// the signal comes in and:
// 0 - config (card index)
// 1 - give me the clocks
// 2 - set value to specified file
//    signal type + new value + # + file path
//
// 4 - start timer with given interval
// 5 - stop timer
// 6 - shared mem key
void rpdThread::performTask(const QString &signal) {
    qDebug() << "Performing task: " << signal;

    if (signal.isEmpty()) {
        qWarning() << "Received empty signal";
        return;
    }

    bool setValueSucces = false;
    QString confirmationMsg;

    QStringList instructions = signal.split(SEPARATOR);
    instructions.removeLast();

    int size = instructions.size();

    // Cycle through instructions
    for (int index = 0; index < size; index++) {

        // Check the first char (instruction type)
        switch (instructions[index][0].toLatin1()) {

                // SIGNAL_CONFIG + SEPARATOR + CLOCKS_PATH + SEPARATOR
            case SIGNAL_CONFIG:
                qDebug() << "Elaborating a CONFIG signal";

                if (!checkRequiredCommandLength(1, index, size)) {
                    qCritical() << "Invalid command! (index out of bounds)";
                    return;
                }

                if (!configure(instructions[++index]))
                    qWarning() << "Configuration failed.";

                break;

                // SIGNAL_READ_CLOCKS + SEPARATOR
            case SIGNAL_READ_CLOCKS:
                qDebug() << "Elaborating a READ_CLOCKS signal";
                readData();
                break;

                // SIGNAL_SET_VALUE + SEPARATOR + VALUE + SEPARATOR + PATH + SEPARATOR
            case SIGNAL_SET_VALUE: {
                qDebug() << "Elaborating a SET_VALUE signal";

                if (!checkRequiredCommandLength(2, index, size)) {
                    qCritical() << "Invalid command! (index out of bounds)";
                    return;
                }

                const QString value = instructions[++index],
                        path = instructions[++index];

                if (setNewValue(path, value)) {
                        setValueSucces = true;
                        confirmationMsg.append(value).append("#");
                }

                break;
            }

                // SIGNAL_TIMER_ON + SEPARATOR + INTERVAL + SEPARATOR
            case SIGNAL_TIMER_ON: {
                qDebug() << "Elaborating a TIMER_ON signal";

                if (!checkRequiredCommandLength(1, index, size)) {
                    qCritical() << "Invalid command! (index out of bounds)";
                    return;
                }

                int inputMillis = instructions[++index].toInt(); // Seconds integer

                if (inputMillis < 1) {
                    qCritical() << "Invalid value TIMER_ON value: " << instructions[index];
                    break;
                }

                qDebug() << "Setting up timer with seconds interval: " << inputMillis;
                timer.start(inputMillis * 1000); // Config and start the timer
                break;
            }

                // SIGNAL_TIMER_OFF + SEPARATOR
            case SIGNAL_TIMER_OFF:
                qDebug() << "Elaborating a TIMER_OFF signal";
                timer.stop();
                break;

            case SIGNAL_SHAREDMEM_KEY: {

                if (!checkRequiredCommandLength(1, index, size)) {
                    qCritical() << "Invalid command! (index out of bounds)";
                    return;
                }

                QString key = instructions[++index];
                qDebug() << "Shared memory key: " << key;
                configureSharedMem(key);
                break;
            }
            default:
                qWarning() << "Unknown signal received: " << signal;
                break;
        }

    }

    if (setValueSucces) {
        QByteArray feedback;
        QDataStream out(&feedback, QIODevice::WriteOnly);

        out << confirmationMsg;

        signalReceiver->write(feedback);
    }

}

bool rpdThread::checkRequiredCommandLength(unsigned required, unsigned currentIndex, unsigned size) {
    if (size <= currentIndex + required)
        return false;

    return  true;
}

bool rpdThread::configure(const QString &filePath) {
    if (!filePath.startsWith("/sys/kernel/debug/dri/")) {
        // The file path is not in whitelisted directories
        // This is a security check to prevent exploiters from reading root files
        qCritical() << "Illegal clocks data path: " << filePath;
        return false;
    }

    QFileInfo clocksFileInfo(filePath);
    if (!clocksFileInfo.exists()) {
        // The indicated clocks file path does not exist
        qCritical() << "Indicated clocks data path does not exist: " << filePath;
        return false;
    }

    if (!clocksFileInfo.isFile()) {
        // The path is a directory, not a file
        qCritical() << "Indicated clocks data path is not a valid file: " << filePath;
        return false;
    }

    qDebug() << "The new clocks data path is " << filePath;
    clocksDataPath = filePath; // Remember the path for the next readings

    // The clocks data file will be copied in /tmp/
    // Let's calculate the destination file path
    QString destination="/tmp/" + clocksFileInfo.fileName();
    // Example: /sys/kernel/debug/dri/0/radeon_pm_info -> /tmp/radeon_pm_info
    qDebug() << clocksDataPath << " will be copied into " << destination;

    system(QString("cp " + clocksDataPath + " " + destination).toStdString().c_str());
    return true;
}

void rpdThread::readData() {
    if (!sharedMem.isAttached() && !sharedMem.attach()) {
        qWarning() << "Shared memory is not attached, can't write data: " << sharedMem.errorString();
        return;
    }

    QFile f(clocksDataPath);
    QByteArray data;
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "Unable to open file " << clocksDataPath;
        return;
    }

    qDebug() << "Reading file: " << clocksDataPath;
    data = f.readAll();
    f.close();

    if (data.isEmpty()) {
        qWarning() << "Unable to read file " << clocksDataPath;
        return;
    }

    if (!sharedMem.lock()) {
        qWarning() << "Shared memory can't be locked, can't write data: " << sharedMem.errorString();
        return;
    }

    char *to = (char*)sharedMem.data();
    if (to == NULL) {
        qWarning() << "Shared memory data pointer is invalid: " << sharedMem.errorString();
        sharedMem.unlock();
        return;
    }

    memcpy(sharedMem.data(), data.constData(), sharedMem.size());
    sharedMem.unlock();
}

bool rpdThread::setNewValue(const QString &filePath, const QString &newValue) {
    if (filePath.isEmpty()) {
        // The specified file path is invalid
        qWarning() << "The file path indicated to be set is empty. Lost value: " << newValue;
        return false;
    }

    if (!filePath.startsWith("/sys/class/drm/")) {
        // The file path is not in whitelisted directories
        // This is a security check to prevent exploiters from writing files as root system wide
        qWarning() << "Illegal path to be set: " << filePath;
        return false;
    }

    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        // The indicated path does not exist
        qWarning() << "The ndicated path to be set does not exist: " << filePath;
        return false;
    }

    if (!fileInfo.isFile()) {
        // The path is a directory, not a file
        qWarning() << "Indicated path to be set is not a valid file: " << filePath;
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        // If the file does not open successfully
        qWarning() << "Failed to open the file to be set: " << filePath;
        return false;
    }

    qDebug() << newValue << " will be written into " << filePath;
    QTextStream stream(&file);
    stream << newValue + "\n";

    if (!file.flush())
        // If writing down the changes fails
        qWarning() << "Failed writing in " << filePath;

    file.close();

    return true;
}

void rpdThread::configureSharedMem(const QString &key) {
    sharedMem.setKey(key);

    if (!sharedMem.isAttached()) {
        qDebug() << "Shared memory is not attached, trying to attach";

        if (!sharedMem.attach())
            qCritical() << "Unable to attach to shared memory:" << sharedMem.errorString();

    }
}
