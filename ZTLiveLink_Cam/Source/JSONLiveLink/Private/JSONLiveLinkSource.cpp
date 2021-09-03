// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

#include "JSONLiveLinkSource.h"

#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "Roles/LiveLinkTransformRole.h"
#include "Roles/LiveLinkTransformTypes.h"
#include "Roles/LiveLinkCameraRole.h"
#include "Roles/LiveLinkCameraTypes.h"

#include "Async/Async.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/RunnableThread.h"
#include "Json.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

#include "Math/UnrealMathUtility.h"

#define LOCTEXT_NAMESPACE "JSONLiveLinkSource"

#define RECV_BUFFER_SIZE 1024 * 1024

FJSONLiveLinkSource::FJSONLiveLinkSource(FIPv4Endpoint InEndpoint)
	: Socket(nullptr), Stopping(false), Thread(nullptr), WaitTime(FTimespan::FromMilliseconds(100))
{
	// defaults
	DeviceEndpoint = InEndpoint;

	SourceStatus = LOCTEXT("SourceStatus_DeviceNotFound", "Device Not Found");
	SourceType = LOCTEXT("JSONLiveLinkSourceType", "ZT_LiveLink");
	SourceMachineName = LOCTEXT("JSONLiveLinkSourceMachineName", "localhost");

	//setup socket
	if (DeviceEndpoint.Address.IsMulticastAddress())
	{
		Socket = FUdpSocketBuilder(TEXT("JSONSOCKET"))
			.AsNonBlocking()
			.AsReusable()
			.BoundToPort(DeviceEndpoint.Port)
			.WithReceiveBufferSize(RECV_BUFFER_SIZE)

			.BoundToAddress(FIPv4Address::Any)
			.JoinedToGroup(DeviceEndpoint.Address)
			.WithMulticastLoopback()
			.WithMulticastTtl(2);
	}
	else
	{
		Socket = FUdpSocketBuilder(TEXT("JSONSOCKET"))
			.AsNonBlocking()
			.AsReusable()
			.BoundToAddress(DeviceEndpoint.Address)
			.BoundToPort(DeviceEndpoint.Port)
			.WithReceiveBufferSize(RECV_BUFFER_SIZE);
	}

	RecvBuffer.SetNumUninitialized(RECV_BUFFER_SIZE);

	if ((Socket != nullptr) && (Socket->GetSocketType() == SOCKTYPE_Datagram))
	{
		SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

		Start();

		SourceStatus = LOCTEXT("SourceStatus_Receiving", "Receiving");
	}
}

FJSONLiveLinkSource::~FJSONLiveLinkSource()
{
	Stop();
	if (Thread != nullptr)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	if (Socket != nullptr)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
	}
}

void FJSONLiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
	Client = InClient;
	SourceGuid = InSourceGuid;
}

bool FJSONLiveLinkSource::IsSourceStillValid() const
{
	// Source is valid if we have a valid thread and socket
	bool bIsSourceValid = !Stopping && Thread != nullptr && Socket != nullptr;
	return bIsSourceValid;
}

bool FJSONLiveLinkSource::RequestSourceShutdown()
{
	Stop();

	return true;
}
// FRunnable interface

void FJSONLiveLinkSource::Start()
{
	ThreadName = "JSON UDP Receiver ";
	ThreadName.AppendInt(FAsyncThreadIndex::GetNext());
	Thread = FRunnableThread::Create(this, *ThreadName, 128 * 1024, TPri_AboveNormal, FPlatformAffinity::GetPoolThreadMask());
}

void FJSONLiveLinkSource::Stop()
{
	Stopping = true;

	if (Thread != nullptr)
    {
        Thread->WaitForCompletion();
        Thread = nullptr;
    }
}

uint32 FJSONLiveLinkSource::Run()
{
	TSharedRef<FInternetAddr> Sender = SocketSubsystem->CreateInternetAddr();

	while (!Stopping)
	{
		if (Socket->Wait(ESocketWaitConditions::WaitForRead, WaitTime))
		{
			uint32 Size;

			while (Socket->HasPendingData(Size))
			{
				int32 Read = 0;

				if (Socket->RecvFrom(RecvBuffer.GetData(), RecvBuffer.Num(), Read, *Sender))
				{
					if (Read > 0)
					{
						TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> ReceivedData = MakeShareable(new TArray<uint8>());
						ReceivedData->SetNumUninitialized(Read);
						memcpy(ReceivedData->GetData(), RecvBuffer.GetData(), Read);
						AsyncTask(ENamedThreads::GameThread, [this, ReceivedData]() { HandleReceivedData(ReceivedData); });
					}
				}
			}
		}
	}
	return 0;
}



void FJSONLiveLinkSource::HandleReceivedData(TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> ReceivedData)
{
	FString JsonString;
	JsonString.Empty(ReceivedData->Num());
	for (uint8& Byte : *ReceivedData.Get())
	{
		JsonString += TCHAR(Byte);
	}

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		for (TPair<FString, TSharedPtr<FJsonValue>>& JsonField : JsonObject->Values)
		{
			FString s = JsonField.Key;
    		s += "@";
    		s += DeviceEndpoint.ToString();
    		FName SubjectName(s);
			//FName SubjectName(*JsonField.Key);
			const TSharedPtr<FJsonObject> CameraObject = JsonField.Value->AsObject();

			bool bCreateSubject = !EncounteredSubjects.Contains(SubjectName);

			if (bCreateSubject)
			{
				FLiveLinkStaticDataStruct StaticDataStruct = FLiveLinkStaticDataStruct(FLiveLinkCameraStaticData::StaticStruct());
				FLiveLinkCameraStaticData& StaticData = *StaticDataStruct.Cast<FLiveLinkCameraStaticData>();
				StaticData.bIsFocalLengthSupported = true;
				StaticData.bIsFocusDistanceSupported = true;
				StaticData.bIsApertureSupported = true;

				Client->PushSubjectStaticData_AnyThread({ SourceGuid, SubjectName }, ULiveLinkCameraRole::StaticClass(), MoveTemp(StaticDataStruct));
				EncounteredSubjects.Add(SubjectName);
				//bCreateSubject = false;
			}
			if (!Stopping) {

				

				//------------------------------------------------------------------------------------------------------------------------------//
				//data reading
				const TArray<TSharedPtr<FJsonValue>>* DataArray;

				FQuat Quaternion(FQuat::Identity);
				FVector Euler(FVector::ZeroVector);
				FVector Translation(FVector::ZeroVector);
				FVector Scale(FVector::ZeroVector);

				FLiveLinkFrameDataStruct CameraFrameDataStruct = FLiveLinkFrameDataStruct(FLiveLinkCameraFrameData::StaticStruct());
				FLiveLinkCameraFrameData& CameraFrameData = *CameraFrameDataStruct.Cast<FLiveLinkCameraFrameData>();
				

				if (CameraObject->TryGetArrayField(TEXT("UserData"), DataArray) && DataArray->Num() == 10)
				{
					double Data0 = (*DataArray)[0]->AsNumber();
					double Data1 = (*DataArray)[1]->AsNumber();
					double Data2 = (*DataArray)[2]->AsNumber();
					double Data3 = (*DataArray)[3]->AsNumber();
					double Data4 = (*DataArray)[4]->AsNumber();
					double Data5 = (*DataArray)[5]->AsNumber();
					double Data6 = (*DataArray)[0]->AsNumber();
					double Data7 = (*DataArray)[1]->AsNumber();
					double Data8 = (*DataArray)[2]->AsNumber();
					double Data9 = (*DataArray)[3]->AsNumber();

					Translation = FVector(Data0, Data1, Data2);	
					Euler = FVector(Data3, Data4, Data5);
					Quaternion = FQuat::MakeFromEuler(Euler);
    				Scale = FVector(1.0, 1.0, 1.0);
    				
				}
				else
				{
					// Invalid Json Format
					UE_LOG(LogTemp, Error, TEXT("Warning Invalid Dynamic JSON Reference"));
					return;
				}

				//------------------------------------------------------------------------------------------------------------------------------//

				CameraFrameData.Transform = FTransform(Quaternion, Translation, Scale);
				CameraFrameData.FocalLength = Data6;
				CameraFrameData.FocusDistance = Data7;
				CameraFrameData.Aperture = Data8;
				Client->PushSubjectFrameData_AnyThread({ SourceGuid, SubjectName }, MoveTemp(CameraFrameDataStruct));
			}
		}
	}

}

#undef LOCTEXT_NAMESPACE
