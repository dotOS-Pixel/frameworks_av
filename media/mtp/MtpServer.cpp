/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#include <cutils/properties.h>

#define LOG_TAG "MtpServer"

#include "MtpDebug.h"
#include "MtpDatabase.h"
#include "MtpProperty.h"
#include "MtpServer.h"
#include "MtpStorage.h"
#include "MtpStringBuffer.h"

#include <linux/usb/f_mtp.h>

namespace android {

static const MtpOperationCode kSupportedOperationCodes[] = {
    MTP_OPERATION_GET_DEVICE_INFO,
    MTP_OPERATION_OPEN_SESSION,
    MTP_OPERATION_CLOSE_SESSION,
    MTP_OPERATION_GET_STORAGE_IDS,
    MTP_OPERATION_GET_STORAGE_INFO,
    MTP_OPERATION_GET_NUM_OBJECTS,
    MTP_OPERATION_GET_OBJECT_HANDLES,
    MTP_OPERATION_GET_OBJECT_INFO,
    MTP_OPERATION_GET_OBJECT,
//    MTP_OPERATION_GET_THUMB,
    MTP_OPERATION_DELETE_OBJECT,
    MTP_OPERATION_SEND_OBJECT_INFO,
    MTP_OPERATION_SEND_OBJECT,
//    MTP_OPERATION_INITIATE_CAPTURE,
//    MTP_OPERATION_FORMAT_STORE,
//    MTP_OPERATION_RESET_DEVICE,
//    MTP_OPERATION_SELF_TEST,
//    MTP_OPERATION_SET_OBJECT_PROTECTION,
//    MTP_OPERATION_POWER_DOWN,
    MTP_OPERATION_GET_DEVICE_PROP_DESC,
    MTP_OPERATION_GET_DEVICE_PROP_VALUE,
    MTP_OPERATION_SET_DEVICE_PROP_VALUE,
    MTP_OPERATION_RESET_DEVICE_PROP_VALUE,
//    MTP_OPERATION_TERMINATE_OPEN_CAPTURE,
//    MTP_OPERATION_MOVE_OBJECT,
//    MTP_OPERATION_COPY_OBJECT,
    MTP_OPERATION_GET_PARTIAL_OBJECT,
//    MTP_OPERATION_INITIATE_OPEN_CAPTURE,
    MTP_OPERATION_GET_OBJECT_PROPS_SUPPORTED,
    MTP_OPERATION_GET_OBJECT_PROP_DESC,
    MTP_OPERATION_GET_OBJECT_PROP_VALUE,
    MTP_OPERATION_SET_OBJECT_PROP_VALUE,
    MTP_OPERATION_GET_OBJECT_PROP_LIST,
//    MTP_OPERATION_SET_OBJECT_PROP_LIST,
//    MTP_OPERATION_GET_INTERDEPENDENT_PROP_DESC,
//    MTP_OPERATION_SEND_OBJECT_PROP_LIST,
    MTP_OPERATION_GET_OBJECT_REFERENCES,
    MTP_OPERATION_SET_OBJECT_REFERENCES,
//    MTP_OPERATION_SKIP,
};

static const MtpEventCode kSupportedEventCodes[] = {
    MTP_EVENT_OBJECT_ADDED,
    MTP_EVENT_OBJECT_REMOVED,
};

MtpServer::MtpServer(int fd, MtpDatabase* database,
                    int fileGroup, int filePerm, int directoryPerm)
    :   mFD(fd),
        mDatabase(database),
        mFileGroup(fileGroup),
        mFilePermission(filePerm),
        mDirectoryPermission(directoryPerm),
        mSessionID(0),
        mSessionOpen(false),
        mSendObjectHandle(kInvalidObjectHandle),
        mSendObjectFormat(0),
        mSendObjectFileSize(0)
{
}

MtpServer::~MtpServer() {
}

void MtpServer::addStorage(const char* filePath, uint64_t reserveSpace) {
    int index = mStorages.size() + 1;
    index |= index << 16;   // set high and low part to our index
    MtpStorage* storage = new MtpStorage(index, filePath, reserveSpace);
    addStorage(storage);
}

MtpStorage* MtpServer::getStorage(MtpStorageID id) {
    if (id == 0)
        return mStorages[0];
    for (int i = 0; i < mStorages.size(); i++) {
        MtpStorage* storage = mStorages[i];
        if (storage->getStorageID() == id)
            return storage;
    }
    return NULL;
}

void MtpServer::run() {
    int fd = mFD;

    LOGV("MtpServer::run fd: %d\n", fd);

    while (1) {
        int ret = mRequest.read(fd);
        if (ret < 0) {
            LOGE("request read returned %d, errno: %d", ret, errno);
            if (errno == ECANCELED) {
                // return to top of loop and wait for next command
                continue;
            }
            break;
        }
        MtpOperationCode operation = mRequest.getOperationCode();
        MtpTransactionID transaction = mRequest.getTransactionID();

        LOGV("operation: %s", MtpDebug::getOperationCodeName(operation));
        mRequest.dump();

        // FIXME need to generalize this
        bool dataIn = (operation == MTP_OPERATION_SEND_OBJECT_INFO
                    || operation == MTP_OPERATION_SET_OBJECT_REFERENCES
                    || operation == MTP_OPERATION_SET_OBJECT_PROP_VALUE
                    || operation == MTP_OPERATION_SET_DEVICE_PROP_VALUE);
        if (dataIn) {
            int ret = mData.read(fd);
            if (ret < 0) {
                LOGE("data read returned %d, errno: %d", ret, errno);
                if (errno == ECANCELED) {
                    // return to top of loop and wait for next command
                    continue;
                }
                break;
            }
            LOGV("received data:");
            mData.dump();
        } else {
            mData.reset();
        }

        if (handleRequest()) {
            if (!dataIn && mData.hasData()) {
                mData.setOperationCode(operation);
                mData.setTransactionID(transaction);
                LOGV("sending data:");
                mData.dump();
                ret = mData.write(fd);
                if (ret < 0) {
                    LOGE("request write returned %d, errno: %d", ret, errno);
                    if (errno == ECANCELED) {
                        // return to top of loop and wait for next command
                        continue;
                    }
                    break;
                }
            }

            mResponse.setTransactionID(transaction);
            LOGV("sending response %04X", mResponse.getResponseCode());
            ret = mResponse.write(fd);
            mResponse.dump();
            if (ret < 0) {
                LOGE("request write returned %d, errno: %d", ret, errno);
                if (errno == ECANCELED) {
                    // return to top of loop and wait for next command
                    continue;
                }
                break;
            }
        } else {
            LOGV("skipping response\n");
        }
    }

    if (mSessionOpen)
        mDatabase->sessionEnded();
}

void MtpServer::sendObjectAdded(MtpObjectHandle handle) {
    if (mSessionOpen) {
        LOGD("sendObjectAdded %d\n", handle);
        mEvent.setEventCode(MTP_EVENT_OBJECT_ADDED);
        mEvent.setTransactionID(mRequest.getTransactionID());
        mEvent.setParameter(1, handle);
        int ret = mEvent.write(mFD);
        LOGD("mEvent.write returned %d\n", ret);
    }
}

void MtpServer::sendObjectRemoved(MtpObjectHandle handle) {
    if (mSessionOpen) {
        LOGD("sendObjectRemoved %d\n", handle);
        mEvent.setEventCode(MTP_EVENT_OBJECT_REMOVED);
        mEvent.setTransactionID(mRequest.getTransactionID());
        mEvent.setParameter(1, handle);
        int ret = mEvent.write(mFD);
        LOGD("mEvent.write returned %d\n", ret);
    }
}

bool MtpServer::handleRequest() {
    MtpOperationCode operation = mRequest.getOperationCode();
    MtpResponseCode response;

    mResponse.reset();

    if (mSendObjectHandle != kInvalidObjectHandle && operation != MTP_OPERATION_SEND_OBJECT) {
        // FIXME - need to delete mSendObjectHandle from the database
        LOGE("expected SendObject after SendObjectInfo");
        mSendObjectHandle = kInvalidObjectHandle;
    }

    switch (operation) {
        case MTP_OPERATION_GET_DEVICE_INFO:
            response = doGetDeviceInfo();
            break;
        case MTP_OPERATION_OPEN_SESSION:
            response = doOpenSession();
            break;
        case MTP_OPERATION_CLOSE_SESSION:
            response = doCloseSession();
            break;
        case MTP_OPERATION_GET_STORAGE_IDS:
            response = doGetStorageIDs();
            break;
         case MTP_OPERATION_GET_STORAGE_INFO:
            response = doGetStorageInfo();
            break;
        case MTP_OPERATION_GET_OBJECT_PROPS_SUPPORTED:
            response = doGetObjectPropsSupported();
            break;
        case MTP_OPERATION_GET_OBJECT_HANDLES:
            response = doGetObjectHandles();
            break;
        case MTP_OPERATION_GET_NUM_OBJECTS:
            response = doGetNumObjects();
            break;
        case MTP_OPERATION_GET_OBJECT_REFERENCES:
            response = doGetObjectReferences();
            break;
        case MTP_OPERATION_SET_OBJECT_REFERENCES:
            response = doSetObjectReferences();
            break;
        case MTP_OPERATION_GET_OBJECT_PROP_VALUE:
            response = doGetObjectPropValue();
            break;
        case MTP_OPERATION_SET_OBJECT_PROP_VALUE:
            response = doSetObjectPropValue();
            break;
        case MTP_OPERATION_GET_DEVICE_PROP_VALUE:
            response = doGetDevicePropValue();
            break;
        case MTP_OPERATION_SET_DEVICE_PROP_VALUE:
            response = doSetDevicePropValue();
            break;
        case MTP_OPERATION_RESET_DEVICE_PROP_VALUE:
            response = doResetDevicePropValue();
            break;
        case MTP_OPERATION_GET_OBJECT_PROP_LIST:
            response = doGetObjectPropList();
            break;
        case MTP_OPERATION_GET_OBJECT_INFO:
            response = doGetObjectInfo();
            break;
        case MTP_OPERATION_GET_OBJECT:
            response = doGetObject();
            break;
        case MTP_OPERATION_GET_PARTIAL_OBJECT:
            response = doGetPartialObject();
            break;
        case MTP_OPERATION_SEND_OBJECT_INFO:
            response = doSendObjectInfo();
            break;
        case MTP_OPERATION_SEND_OBJECT:
            response = doSendObject();
            break;
        case MTP_OPERATION_DELETE_OBJECT:
            response = doDeleteObject();
            break;
        case MTP_OPERATION_GET_OBJECT_PROP_DESC:
            response = doGetObjectPropDesc();
            break;
        case MTP_OPERATION_GET_DEVICE_PROP_DESC:
            response = doGetDevicePropDesc();
            break;
        default:
            LOGE("got unsupported command %s", MtpDebug::getOperationCodeName(operation));
            response = MTP_RESPONSE_OPERATION_NOT_SUPPORTED;
            break;
    }

    if (response == MTP_RESPONSE_TRANSACTION_CANCELLED)
        return false;
    mResponse.setResponseCode(response);
    return true;
}

MtpResponseCode MtpServer::doGetDeviceInfo() {
    MtpStringBuffer   string;
    char prop_value[PROPERTY_VALUE_MAX];

    MtpObjectFormatList* playbackFormats = mDatabase->getSupportedPlaybackFormats();
    MtpObjectFormatList* captureFormats = mDatabase->getSupportedCaptureFormats();
    MtpDevicePropertyList* deviceProperties = mDatabase->getSupportedDeviceProperties();

    // fill in device info
    mData.putUInt16(MTP_STANDARD_VERSION);
    mData.putUInt32(6); // MTP Vendor Extension ID
    mData.putUInt16(MTP_STANDARD_VERSION);
    string.set("microsoft.com: 1.0;");
    mData.putString(string); // MTP Extensions
    mData.putUInt16(0); //Functional Mode
    mData.putAUInt16(kSupportedOperationCodes,
            sizeof(kSupportedOperationCodes) / sizeof(uint16_t)); // Operations Supported
    mData.putAUInt16(kSupportedEventCodes,
            sizeof(kSupportedEventCodes) / sizeof(uint16_t)); // Events Supported
    mData.putAUInt16(deviceProperties); // Device Properties Supported
    mData.putAUInt16(captureFormats); // Capture Formats
    mData.putAUInt16(playbackFormats);  // Playback Formats
    // FIXME
    string.set("Google, Inc.");
    mData.putString(string);   // Manufacturer

    property_get("ro.product.model", prop_value, "MTP Device");
    string.set(prop_value);
    mData.putString(string);   // Model
    string.set("1.0");
    mData.putString(string);   // Device Version

    property_get("ro.serialno", prop_value, "????????");
    string.set(prop_value);
    mData.putString(string);   // Serial Number

    delete playbackFormats;
    delete captureFormats;
    delete deviceProperties;

    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doOpenSession() {
    if (mSessionOpen) {
        mResponse.setParameter(1, mSessionID);
        return MTP_RESPONSE_SESSION_ALREADY_OPEN;
    }
    mSessionID = mRequest.getParameter(1);
    mSessionOpen = true;

    mDatabase->sessionStarted();

    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doCloseSession() {
    if (!mSessionOpen)
        return MTP_RESPONSE_SESSION_NOT_OPEN;
    mSessionID = 0;
    mSessionOpen = false;
    mDatabase->sessionEnded();
    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doGetStorageIDs() {
    if (!mSessionOpen)
        return MTP_RESPONSE_SESSION_NOT_OPEN;

    int count = mStorages.size();
    mData.putUInt32(count);
    for (int i = 0; i < count; i++)
        mData.putUInt32(mStorages[i]->getStorageID());

    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doGetStorageInfo() {
    MtpStringBuffer   string;

    if (!mSessionOpen)
        return MTP_RESPONSE_SESSION_NOT_OPEN;
    MtpStorageID id = mRequest.getParameter(1);
    MtpStorage* storage = getStorage(id);
    if (!storage)
        return MTP_RESPONSE_INVALID_STORAGE_ID;

    mData.putUInt16(storage->getType());
    mData.putUInt16(storage->getFileSystemType());
    mData.putUInt16(storage->getAccessCapability());
    mData.putUInt64(storage->getMaxCapacity());
    mData.putUInt64(storage->getFreeSpace());
    mData.putUInt32(1024*1024*1024); // Free Space in Objects
    string.set(storage->getDescription());
    mData.putString(string);
    mData.putEmptyString();   // Volume Identifier

    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doGetObjectPropsSupported() {
    if (!mSessionOpen)
        return MTP_RESPONSE_SESSION_NOT_OPEN;
    MtpObjectFormat format = mRequest.getParameter(1);
    MtpObjectPropertyList* properties = mDatabase->getSupportedObjectProperties(format);
    mData.putAUInt16(properties);
    delete properties;
    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doGetObjectHandles() {
    if (!mSessionOpen)
        return MTP_RESPONSE_SESSION_NOT_OPEN;
    MtpStorageID storageID = mRequest.getParameter(1);      // 0xFFFFFFFF for all storage
    MtpObjectFormat format = mRequest.getParameter(2);      // 0 for all formats
    MtpObjectHandle parent = mRequest.getParameter(3);      // 0xFFFFFFFF for objects with no parent
                                                            // 0x00000000 for all objects?
    if (parent == 0xFFFFFFFF)
        parent = 0;

    MtpObjectHandleList* handles = mDatabase->getObjectList(storageID, format, parent);
    mData.putAUInt32(handles);
    delete handles;
    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doGetNumObjects() {
    if (!mSessionOpen)
        return MTP_RESPONSE_SESSION_NOT_OPEN;
    MtpStorageID storageID = mRequest.getParameter(1);      // 0xFFFFFFFF for all storage
    MtpObjectFormat format = mRequest.getParameter(2);      // 0 for all formats
    MtpObjectHandle parent = mRequest.getParameter(3);      // 0xFFFFFFFF for objects with no parent
                                                            // 0x00000000 for all objects?
    if (parent == 0xFFFFFFFF)
        parent = 0;

    int count = mDatabase->getNumObjects(storageID, format, parent);
    if (count >= 0) {
        mResponse.setParameter(1, count);
        return MTP_RESPONSE_OK;
    } else {
        mResponse.setParameter(1, 0);
        return MTP_RESPONSE_INVALID_OBJECT_HANDLE;
    }
}

MtpResponseCode MtpServer::doGetObjectReferences() {
    if (!mSessionOpen)
        return MTP_RESPONSE_SESSION_NOT_OPEN;
    MtpStorageID handle = mRequest.getParameter(1);

    // FIXME - check for invalid object handle
    MtpObjectHandleList* handles = mDatabase->getObjectReferences(handle);
    if (handles) {
        mData.putAUInt32(handles);
        delete handles;
    } else {
        mData.putEmptyArray();
    }
    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doSetObjectReferences() {
    if (!mSessionOpen)
        return MTP_RESPONSE_SESSION_NOT_OPEN;
    MtpStorageID handle = mRequest.getParameter(1);
    MtpObjectHandleList* references = mData.getAUInt32();
    MtpResponseCode result = mDatabase->setObjectReferences(handle, references);
    delete references;
    return result;
}

MtpResponseCode MtpServer::doGetObjectPropValue() {
    MtpObjectHandle handle = mRequest.getParameter(1);
    MtpObjectProperty property = mRequest.getParameter(2);
    LOGD("GetObjectPropValue %d %s\n", handle,
            MtpDebug::getObjectPropCodeName(property));

    return mDatabase->getObjectPropertyValue(handle, property, mData);
}

MtpResponseCode MtpServer::doSetObjectPropValue() {
    MtpObjectHandle handle = mRequest.getParameter(1);
    MtpObjectProperty property = mRequest.getParameter(2);
    LOGD("SetObjectPropValue %d %s\n", handle,
            MtpDebug::getObjectPropCodeName(property));

    return mDatabase->setObjectPropertyValue(handle, property, mData);
}

MtpResponseCode MtpServer::doGetDevicePropValue() {
    MtpDeviceProperty property = mRequest.getParameter(1);
    LOGD("GetDevicePropValue %s\n",
            MtpDebug::getDevicePropCodeName(property));

    return mDatabase->getDevicePropertyValue(property, mData);
}

MtpResponseCode MtpServer::doSetDevicePropValue() {
    MtpDeviceProperty property = mRequest.getParameter(1);
    LOGD("SetDevicePropValue %s\n",
            MtpDebug::getDevicePropCodeName(property));

    return mDatabase->setDevicePropertyValue(property, mData);
}

MtpResponseCode MtpServer::doResetDevicePropValue() {
    MtpDeviceProperty property = mRequest.getParameter(1);
    LOGD("ResetDevicePropValue %s\n",
            MtpDebug::getDevicePropCodeName(property));

    return mDatabase->resetDeviceProperty(property);
}

MtpResponseCode MtpServer::doGetObjectPropList() {

    MtpObjectHandle handle = mRequest.getParameter(1);
    // use uint32_t so we can support 0xFFFFFFFF
    uint32_t format = mRequest.getParameter(2);
    uint32_t property = mRequest.getParameter(3);
    int groupCode = mRequest.getParameter(4);
    int depth = mRequest.getParameter(5);
   LOGD("GetObjectPropList %d format: %s property: %s group: %d depth: %d\n",
            handle, MtpDebug::getFormatCodeName(format),
            MtpDebug::getObjectPropCodeName(property), groupCode, depth);

    return mDatabase->getObjectPropertyList(handle, format, property, groupCode, depth, mData);
}

MtpResponseCode MtpServer::doGetObjectInfo() {
    MtpObjectHandle handle = mRequest.getParameter(1);
    return mDatabase->getObjectInfo(handle, mData);
}

MtpResponseCode MtpServer::doGetObject() {
    MtpObjectHandle handle = mRequest.getParameter(1);
    MtpString pathBuf;
    int64_t fileLength;
    MtpObjectFormat format;
    int result = mDatabase->getObjectFilePath(handle, pathBuf, fileLength, format);
    if (result != MTP_RESPONSE_OK)
        return result;

    const char* filePath = (const char *)pathBuf;
    mtp_file_range  mfr;
    mfr.fd = open(filePath, O_RDONLY);
    if (mfr.fd < 0) {
        return MTP_RESPONSE_GENERAL_ERROR;
    }
    mfr.offset = 0;
    mfr.length = fileLength;

    // send data header
    mData.setOperationCode(mRequest.getOperationCode());
    mData.setTransactionID(mRequest.getTransactionID());
    mData.writeDataHeader(mFD, fileLength + MTP_CONTAINER_HEADER_SIZE);

    // then transfer the file
    int ret = ioctl(mFD, MTP_SEND_FILE, (unsigned long)&mfr);
    close(mfr.fd);
    if (ret < 0) {
        if (errno == ECANCELED)
            return MTP_RESPONSE_TRANSACTION_CANCELLED;
        else
            return MTP_RESPONSE_GENERAL_ERROR;
    }
    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doGetPartialObject() {
    MtpObjectHandle handle = mRequest.getParameter(1);
    uint32_t offset = mRequest.getParameter(2);
    uint32_t length = mRequest.getParameter(3);
    MtpString pathBuf;
    int64_t fileLength;
    MtpObjectFormat format;
    int result = mDatabase->getObjectFilePath(handle, pathBuf, fileLength, format);
    if (result != MTP_RESPONSE_OK)
        return result;
    if (offset + length > fileLength)
        length = fileLength - offset;

    const char* filePath = (const char *)pathBuf;
    mtp_file_range  mfr;
    mfr.fd = open(filePath, O_RDONLY);
    if (mfr.fd < 0) {
        return MTP_RESPONSE_GENERAL_ERROR;
    }
    mfr.offset = offset;
    mfr.length = length;
    mResponse.setParameter(1, length);

    // send data header
    mData.setOperationCode(mRequest.getOperationCode());
    mData.setTransactionID(mRequest.getTransactionID());
    mData.writeDataHeader(mFD, length + MTP_CONTAINER_HEADER_SIZE);

    // then transfer the file
    int ret = ioctl(mFD, MTP_SEND_FILE, (unsigned long)&mfr);
    close(mfr.fd);
    if (ret < 0) {
        if (errno == ECANCELED)
            return MTP_RESPONSE_TRANSACTION_CANCELLED;
        else
            return MTP_RESPONSE_GENERAL_ERROR;
    }
    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doSendObjectInfo() {
    MtpString path;
    MtpStorageID storageID = mRequest.getParameter(1);
    MtpStorage* storage = getStorage(storageID);
    MtpObjectHandle parent = mRequest.getParameter(2);
    if (!storage)
        return MTP_RESPONSE_INVALID_STORAGE_ID;

    // special case the root
    if (parent == MTP_PARENT_ROOT) {
        path = storage->getPath();
        parent = 0;
    } else {
        int64_t length;
        MtpObjectFormat format;
        int result = mDatabase->getObjectFilePath(parent, path, length, format);
        if (result != MTP_RESPONSE_OK)
            return result;
        if (format != MTP_FORMAT_ASSOCIATION)
            return MTP_RESPONSE_INVALID_PARENT_OBJECT;
    }

    // read only the fields we need
    mData.getUInt32();  // storage ID
    MtpObjectFormat format = mData.getUInt16();
    mData.getUInt16();  // protection status
    mSendObjectFileSize = mData.getUInt32();
    mData.getUInt16();  // thumb format
    mData.getUInt32();  // thumb compressed size
    mData.getUInt32();  // thumb pix width
    mData.getUInt32();  // thumb pix height
    mData.getUInt32();  // image pix width
    mData.getUInt32();  // image pix height
    mData.getUInt32();  // image bit depth
    mData.getUInt32();  // parent
    uint16_t associationType = mData.getUInt16();
    uint32_t associationDesc = mData.getUInt32();   // association desc
    mData.getUInt32();  // sequence number
    MtpStringBuffer name, created, modified;
    mData.getString(name);    // file name
    mData.getString(created);      // date created
    mData.getString(modified);     // date modified
    // keywords follow

    LOGD("name: %s format: %04X\n", (const char *)name, format);
    time_t modifiedTime;
    if (!parseDateTime(modified, modifiedTime))
        modifiedTime = 0;

    if (path[path.size() - 1] != '/')
        path += "/";
    path += (const char *)name;

    // check space first
    if (mSendObjectFileSize > storage->getFreeSpace())
        return MTP_RESPONSE_STORAGE_FULL;

    MtpObjectHandle handle = mDatabase->beginSendObject((const char*)path,
            format, parent, storageID, mSendObjectFileSize, modifiedTime);
    if (handle == kInvalidObjectHandle) {
        return MTP_RESPONSE_GENERAL_ERROR;
    }

  if (format == MTP_FORMAT_ASSOCIATION) {
        mode_t mask = umask(0);
        int ret = mkdir((const char *)path, mDirectoryPermission);
        umask(mask);
        if (ret && ret != -EEXIST)
            return MTP_RESPONSE_GENERAL_ERROR;
        chown((const char *)path, getuid(), mFileGroup);
    } else {
        mSendObjectFilePath = path;
        // save the handle for the SendObject call, which should follow
        mSendObjectHandle = handle;
        mSendObjectFormat = format;
    }

    mResponse.setParameter(1, storageID);
    mResponse.setParameter(2, parent);
    mResponse.setParameter(3, handle);

    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doSendObject() {
    MtpResponseCode result = MTP_RESPONSE_OK;
    mode_t mask;
    int ret;
    uint64_t actualSize = -1;

    if (mSendObjectHandle == kInvalidObjectHandle) {
        LOGE("Expected SendObjectInfo before SendObject");
        result = MTP_RESPONSE_NO_VALID_OBJECT_INFO;
        goto done;
    }

    // read the header
    ret = mData.readDataHeader(mFD);
    // FIXME - check for errors here.

    // reset so we don't attempt to send this back
    mData.reset();

    mtp_file_range  mfr;
    mfr.fd = open(mSendObjectFilePath, O_RDWR | O_CREAT | O_TRUNC);
    if (mfr.fd < 0) {
        result = MTP_RESPONSE_GENERAL_ERROR;
        goto done;
    }
    fchown(mfr.fd, getuid(), mFileGroup);
    // set permissions
    mask = umask(0);
    fchmod(mfr.fd, mFilePermission);
    umask(mask);

    mfr.offset = 0;
    mfr.length = mSendObjectFileSize;

    LOGD("receiving %s\n", (const char *)mSendObjectFilePath);
    // transfer the file
    ret = ioctl(mFD, MTP_RECEIVE_FILE, (unsigned long)&mfr);
    close(mfr.fd);

    LOGV("MTP_RECEIVE_FILE returned %d", ret);

    if (ret < 0) {
        unlink(mSendObjectFilePath);
        if (errno == ECANCELED)
            result = MTP_RESPONSE_TRANSACTION_CANCELLED;
        else
            result = MTP_RESPONSE_GENERAL_ERROR;
    } else if (mSendObjectFileSize == 0xFFFFFFFF) {
        // actual size is likely > 4 gig so stat the file to compute actual length
        struct stat s;
        if (lstat(mSendObjectFilePath, &s) == 0) {
            actualSize = s.st_size;
            LOGD("actualSize: %lld\n", actualSize);
        }
    }

done:
    mDatabase->endSendObject(mSendObjectFilePath, mSendObjectHandle, mSendObjectFormat,
            actualSize, result == MTP_RESPONSE_OK);
    mSendObjectHandle = kInvalidObjectHandle;
    mSendObjectFormat = 0;
    return result;
}

static void deleteRecursive(const char* path) {
    char pathbuf[PATH_MAX];
    int pathLength = strlen(path);
    if (pathLength >= sizeof(pathbuf) - 1) {
        LOGE("path too long: %s\n", path);
    }
    strcpy(pathbuf, path);
    if (pathbuf[pathLength - 1] != '/') {
        pathbuf[pathLength++] = '/';
    }
    char* fileSpot = pathbuf + pathLength;
    int pathRemaining = sizeof(pathbuf) - pathLength - 1;

    DIR* dir = opendir(path);
    if (!dir) {
        LOGE("opendir %s failed: %s", path, strerror(errno));
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir))) {
        const char* name = entry->d_name;

        // ignore "." and ".."
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
            continue;
        }

        int nameLength = strlen(name);
        if (nameLength > pathRemaining) {
            LOGE("path %s/%s too long\n", path, name);
            continue;
        }
        strcpy(fileSpot, name);

        int type = entry->d_type;
        if (entry->d_type == DT_DIR) {
            deleteRecursive(pathbuf);
            rmdir(pathbuf);
        } else {
            unlink(pathbuf);
        }
    }
    closedir(dir);
}

static void deletePath(const char* path) {
    struct stat statbuf;
    if (stat(path, &statbuf) == 0) {
        if (S_ISDIR(statbuf.st_mode)) {
            deleteRecursive(path);
            rmdir(path);
        } else {
            unlink(path);
        }
    } else {
        LOGE("deletePath stat failed for %s: %s", path, strerror(errno));
    }
}

MtpResponseCode MtpServer::doDeleteObject() {
    MtpObjectHandle handle = mRequest.getParameter(1);
    MtpObjectFormat format = mRequest.getParameter(2);
    // FIXME - support deleting all objects if handle is 0xFFFFFFFF
    // FIXME - implement deleting objects by format

    MtpString filePath;
    int64_t fileLength;
    int result = mDatabase->getObjectFilePath(handle, filePath, fileLength, format);
    if (result == MTP_RESPONSE_OK) {
        LOGV("deleting %s", (const char *)filePath);
        deletePath((const char *)filePath);
        return mDatabase->deleteFile(handle);
    } else {
        return result;
    }
}

MtpResponseCode MtpServer::doGetObjectPropDesc() {
    MtpObjectProperty propCode = mRequest.getParameter(1);
    MtpObjectFormat format = mRequest.getParameter(2);
    LOGD("GetObjectPropDesc %s %s\n", MtpDebug::getObjectPropCodeName(propCode),
                                        MtpDebug::getFormatCodeName(format));
    MtpProperty* property = mDatabase->getObjectPropertyDesc(propCode, format);
    if (!property)
        return MTP_RESPONSE_OBJECT_PROP_NOT_SUPPORTED;
    property->write(mData);
    delete property;
    return MTP_RESPONSE_OK;
}

MtpResponseCode MtpServer::doGetDevicePropDesc() {
    MtpDeviceProperty propCode = mRequest.getParameter(1);
    LOGD("GetDevicePropDesc %s\n", MtpDebug::getDevicePropCodeName(propCode));
    MtpProperty* property = mDatabase->getDevicePropertyDesc(propCode);
    if (!property)
        return MTP_RESPONSE_DEVICE_PROP_NOT_SUPPORTED;
    property->write(mData);
    delete property;
    return MTP_RESPONSE_OK;
}

}  // namespace android
