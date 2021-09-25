// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

#include "JSONLiveLinkSource.h"

#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "Roles/LiveLinkBasicRole.h"
#include "Roles/LiveLinkBasicTypes.h"

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

//------------------------------------------------------------------------------------------------------------------------------//


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
			const TSharedPtr<FJsonObject> BasicObject = JsonField.Value->AsObject();

			bool bCreateSubject = !EncounteredSubjects.Contains(SubjectName);

			if (bCreateSubject)
			{
				FLiveLinkStaticDataStruct UserStaticDataStruct = FLiveLinkStaticDataStruct(FLiveLinkBaseStaticData::StaticStruct());
				FLiveLinkBaseStaticData& UserStaticData = *UserStaticDataStruct.Cast<FLiveLinkBaseStaticData>();
				UserStaticData.PropertyNames.SetNumUninitialized(10);

				UserStaticData.PropertyNames[0] = FName("userdata0");
				UserStaticData.PropertyNames[1] = FName("userdata1");
				UserStaticData.PropertyNames[2] = FName("userdata2");
				UserStaticData.PropertyNames[3] = FName("userdata3");
				UserStaticData.PropertyNames[4] = FName("userdata4");
				UserStaticData.PropertyNames[5] = FName("userdata5");
				UserStaticData.PropertyNames[6] = FName("userdata6");
				UserStaticData.PropertyNames[7] = FName("userdata7");
				UserStaticData.PropertyNames[8] = FName("userdata8");
				UserStaticData.PropertyNames[9] = FName("userdata9");

				Client->PushSubjectStaticData_AnyThread({ SourceGuid, SubjectName }, ULiveLinkBasicRole::StaticClass(), MoveTemp(UserStaticDataStruct));
				EncounteredSubjects.Add(SubjectName);
				//bCreateSubject = false;
			}
			if (!Stopping) {

				

				//------------------------------------------------------------------------------------------------------------------------------//
				//data reading
				const TArray<TSharedPtr<FJsonValue>>* DataArray;

				FLiveLinkFrameDataStruct UserFrameDataStruct = FLiveLinkFrameDataStruct(FLiveLinkBaseFrameData::StaticStruct());
				FLiveLinkBaseFrameData& UserFrameData = *UserFrameDataStruct.Cast<FLiveLinkBaseFrameData>();
				UserFrameData.PropertyValues.SetNumUninitialized(10);

				if (BasicObject->TryGetArrayField(TEXT("UserData"), DataArray) && DataArray->Num() == 10)
				{
					UserFrameData.PropertyValues[0] = (*DataArray)[0]->AsNumber();
					UserFrameData.PropertyValues[1] = (*DataArray)[1]->AsNumber();
					UserFrameData.PropertyValues[2] = (*DataArray)[2]->AsNumber();
					UserFrameData.PropertyValues[3] = (*DataArray)[3]->AsNumber();
					UserFrameData.PropertyValues[4] = (*DataArray)[4]->AsNumber();
					UserFrameData.PropertyValues[5] = (*DataArray)[5]->AsNumber();
					UserFrameData.PropertyValues[6] = (*DataArray)[6]->AsNumber();
					UserFrameData.PropertyValues[7] = (*DataArray)[7]->AsNumber();
					UserFrameData.PropertyValues[8] = (*DataArray)[8]->AsNumber();
					UserFrameData.PropertyValues[9] = (*DataArray)[9]->AsNumber();

				}
				else
				{
					// Invalid Json Format
					UE_LOG(LogTemp, Error, TEXT("Warning Invalid Dynamic JSON Reference"));
					return;
				}

				//------------------------------------------------------------------------------------------------------------------------------//

				Client->PushSubjectFrameData_AnyThread({ SourceGuid, SubjectName }, MoveTemp(UserFrameDataStruct));
			}
		}
	}

}

#undef LOCTEXT_NAMESPACE
