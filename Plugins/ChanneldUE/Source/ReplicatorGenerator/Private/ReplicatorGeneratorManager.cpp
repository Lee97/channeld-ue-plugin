﻿#include "ReplicatorGeneratorManager.h"

#include "ReplicatorGeneratorDefinition.h"
#include "ReplicatorGeneratorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/AssetManager.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Character.h"
#include "HAL/FileManagerGeneric.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Replication/ChanneldReplicationComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogChanneldRepGenerator);

FReplicatorGeneratorManager::FReplicatorGeneratorManager()
{
	CodeGenerator = new FReplicatorCodeGenerator();
}

FReplicatorGeneratorManager::~FReplicatorGeneratorManager()
{
	if (CodeGenerator != nullptr)
	{
		delete CodeGenerator;
		CodeGenerator = nullptr;
	}
}

FReplicatorGeneratorManager& FReplicatorGeneratorManager::Get()
{
	static FReplicatorGeneratorManager Singleton;
	return Singleton;
}

FString FReplicatorGeneratorManager::GetDefaultModuleDir()
{
	if (DefaultModuleDir.IsEmpty())
	{
		DefaultModuleDir = ChanneldReplicatorGeneratorUtils::GetDefaultModuleDir();
		FPaths::NormalizeDirectoryName(DefaultModuleDir);
	}
	return DefaultModuleDir;
}

FString FReplicatorGeneratorManager::GetReplicatorStorageDir()
{
	if (ReplicatorStorageDir.IsEmpty())
	{
		ReplicatorStorageDir = GetDefaultModuleDir() / GenManager_GeneratedCodeDir;
		FPaths::NormalizeDirectoryName(ReplicatorStorageDir);
	}
	return ReplicatorStorageDir;
}

FString FReplicatorGeneratorManager::GetDefaultProtoPackageName() const
{
	return GenManager_DefaultProtoPackageName;
}

FString FReplicatorGeneratorManager::GetDefaultModuleName()
{
	return FPaths::GetBaseFilename(GetDefaultModuleDir());
}

bool FReplicatorGeneratorManager::HeaderFilesCanBeFound(const UClass* TargetClass)
{
	const FString TargetHeadFilePath = CodeGenerator->GetClassHeadFilePath(TargetClass->GetPrefixCPP() + TargetClass->GetName());
	return !TargetHeadFilePath.IsEmpty();
}

bool FReplicatorGeneratorManager::IsIgnoredActor(const UClass* TargetClass)
{
	return IgnoreActorClasses.Contains(TargetClass) || IgnoreActorClassPaths.Contains(TargetClass->GetPathName());
}

void FReplicatorGeneratorManager::StartGenerateReplicator()
{
	CodeGenerator->RefreshModuleInfoByClassName();
}

void FReplicatorGeneratorManager::StopGenerateReplicator()
{
}

bool FReplicatorGeneratorManager::GeneratedReplicators(const TArray<const UClass*>& TargetClasses, const FString GoPackageImportPathPrefix)
{
	UE_LOG(LogChanneldRepGenerator, Display, TEXT("Start generating %d replicators"), TargetClasses.Num());

	TArray<FString> IncludeActorCodes, RegisterReplicatorCodes;

	FGeneratedCodeBundle ReplicatorCodeBundle;

	const FString ProtoPackageName = GetDefaultProtoPackageName();
	const FString GoPackageImportPath = GoPackageImportPathPrefix + ProtoPackageName;
	CodeGenerator->Generate(
		TargetClasses,
		GetDefaultModuleDir(),
		ProtoPackageName,
		(GoPackageImportPathPrefix + ProtoPackageName),
		ReplicatorCodeBundle
	);
	FString Message;

	// Generate type definitions file
	WriteCodeFile(GetReplicatorStorageDir() / GenManager_TypeDefinitionHeadFile, ReplicatorCodeBundle.TypeDefinitionsHeadCode, Message);
	WriteCodeFile(GetReplicatorStorageDir() / GenManager_TypeDefinitionCppFile, ReplicatorCodeBundle.TypeDefinitionsCppCode, Message);

	// Generate replicator code file
	for (FReplicatorCode& ReplicatorCode : ReplicatorCodeBundle.ReplicatorCodes)
	{
		WriteCodeFile(GetReplicatorStorageDir() / ReplicatorCode.HeadFileName, ReplicatorCode.HeadCode, Message);
		WriteCodeFile(GetReplicatorStorageDir() / ReplicatorCode.CppFileName, ReplicatorCode.CppCode, Message);
		WriteProtoFile(GetReplicatorStorageDir() / ReplicatorCode.ProtoFileName, ReplicatorCode.ProtoDefinitionsFile, Message);
		UE_LOG(
			LogChanneldRepGenerator,
			Verbose,
			TEXT("The replicator for the target class [%s] was generated successfully.\n    Package path: %s\n    Head file: %s\n    CPP file: %s\n    Proto file: %s\n"),
			*ReplicatorCode.ActorDecorator->GetOriginActorName(),
			*ReplicatorCode.ActorDecorator->GetPackagePathName(),
			*ReplicatorCode.HeadFileName,
			*ReplicatorCode.CppFileName,
			*ReplicatorCode.ProtoFileName
		);
	}
	// Generate replicator registration code file
	WriteCodeFile(GetReplicatorStorageDir() / GenManager_RepRegistrationHeadFile, ReplicatorCodeBundle.ReplicatorRegistrationHeadCode, Message);

	// Generate global struct declarations file and proto definitions file
	WriteCodeFile(GetReplicatorStorageDir() / GenManager_GlobalStructHeaderFile, ReplicatorCodeBundle.GlobalStructCodes, Message);
	WriteProtoFile(GetReplicatorStorageDir() / GenManager_GlobalStructProtoFile, ReplicatorCodeBundle.GlobalStructProtoDefinitions, Message);

	// Generate channel data processor code file
	const FString DefaultModuleName = GetDefaultModuleName();
	WriteCodeFile(GetReplicatorStorageDir() / TEXT("ChannelData_") + DefaultModuleName + CodeGen_HeadFileExtension, ReplicatorCodeBundle.ChannelDataProcessorHeadCode, Message);
	WriteProtoFile(GetReplicatorStorageDir() / TEXT("ChannelData_") + DefaultModuleName + CodeGen_ProtoFileExtension, ReplicatorCodeBundle.ChannelDataProtoDefsFile, Message);

	UE_LOG(
		LogChanneldRepGenerator,
		Display,
		TEXT("The generation of replicators is completed, %d replicators need to be generated, a total of %d replicators are generated"),
		TargetClasses.Num(), ReplicatorCodeBundle.ReplicatorCodes.Num()
	);

	// Save the generated manifest file
	FGeneratedManifest Manifest;
	Manifest.GeneratedTime = FDateTime::Now();
	Manifest.ProtoPackageName = ProtoPackageName;
	if (SaveGeneratedManifest(Manifest, Message))
	{
		UE_LOG(LogChanneldRepGenerator, Error, TEXT("Failed to save the generated manifest file, error message: %s"), *Message);
		return false;
	}

	return true;
}

bool FReplicatorGeneratorManager::WriteCodeFile(const FString& FilePath, const FString& Code, FString& ResultMessage)
{
	bool bSuccess = FFileHelper::SaveStringToFile(Code, *FilePath);
	return bSuccess;
}

bool FReplicatorGeneratorManager::WriteProtoFile(const FString& FilePath, const FString& ProtoContent, FString& ResultMessage)
{
	return WriteCodeFile(FilePath, ProtoContent, ResultMessage);
}

TArray<FString> FReplicatorGeneratorManager::GetGeneratedTargetClasses()
{
	TArray<FString> HeadFiles;
	IFileManager::Get().FindFiles(HeadFiles, *GetReplicatorStorageDir(), *CodeGen_HeadFileExtension);

	TArray<FString> Result;
	for (const FString& HeadFile : HeadFiles)
	{
		FRegexPattern MatherPatter(FString(TEXT(R"EOF(^Channeld(\w+)Replicator\.h$)EOF")));
		FRegexMatcher Matcher(MatherPatter, HeadFile);
		if (Matcher.FindNext())
		{
			FString CaptureString = Matcher.GetCaptureGroup(1);
			Result.Add(CaptureString);
		}
	}
	return Result;
}

TArray<FString> FReplicatorGeneratorManager::GetGeneratedProtoFiles()
{
	TArray<FString> AllCodeFiles;
	IFileManager::Get().FindFiles(AllCodeFiles, *GetReplicatorStorageDir(), *CodeGen_ProtoFileExtension);
	return AllCodeFiles;
}

void FReplicatorGeneratorManager::RemoveGeneratedReplicator(const FString& ClassName)
{
	IPlatformFile& FileManager = FPlatformFileManager::Get().GetPlatformFile();
	FileManager.DeleteFile(*FString::Printf(TEXT("Channeld%sReplicator%s"), *ClassName, *CodeGen_CppFileExtension));
	FileManager.DeleteFile(*(FString::Printf(TEXT("Channeld%sReplicator%s"), *ClassName, *CodeGen_HeadFileExtension)));
	FileManager.DeleteFile(*(ClassName + CodeGen_ProtoFileExtension));
	FileManager.DeleteFile(*(ClassName + CodeGen_ProtoPbHeadExtension));
	FileManager.DeleteFile(*(ClassName + CodeGen_ProtoPbCPPExtension));
}

void FReplicatorGeneratorManager::RemoveGeneratedReplicators(const TArray<FString>& ClassNames)
{
	for (const FString& ClassName : ClassNames)
	{
		RemoveGeneratedReplicator(ClassName);
	}
}

void FReplicatorGeneratorManager::RemoveGeneratedCodeFiles()
{
	TArray<FString> AllCodeFiles;
	IFileManager::Get().FindFiles(AllCodeFiles, *GetReplicatorStorageDir());
	for (const FString& FilePath : AllCodeFiles)
	{
		IFileManager::Get().Delete(*(GetReplicatorStorageDir() / FilePath));
	}
}

inline void FReplicatorGeneratorManager::EnsureReplicatorGeneratedIntermediateDir()
{
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*GenManager_IntermediateDir))
	{
		FileManager.MakeDirectory(*GenManager_IntermediateDir, true);
	}
}

bool FReplicatorGeneratorManager::LoadLatestGeneratedManifest(FGeneratedManifest& Result, FString& Message) const
{
	return LoadLatestGeneratedManifest(GenManager_GeneratedManifestFilePath, Result, Message);
}

bool FReplicatorGeneratorManager::LoadLatestGeneratedManifest(const FString& Filename, FGeneratedManifest& Result, FString& Message) const
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Filename))
	{
		Message = FString::Printf(TEXT("Unable to load GeneratedManifest: %s"), *Filename);
		return false;
	}

	TSharedPtr<FJsonObject> RootObject = TSharedPtr<FJsonObject>();
	TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);

	if (!FJsonSerializer::Deserialize(Reader, RootObject))
	{
		Message = FString::Printf(TEXT("GeneratedManifest is malformed: %s"), *Filename);
		return false;
	}

	double GeneratedTime;
	if (!RootObject->TryGetNumberField(TEXT("GeneratedTime"), GeneratedTime))
	{
		UE_LOG(LogChanneldRepGenerator, Warning, TEXT("Unable to find field 'GeneratedTime'"));
	}
	Result.GeneratedTime = FDateTime::FromUnixTimestamp(GeneratedTime);

	if (!RootObject->TryGetStringField(TEXT("ProtoPackageName"), Result.ProtoPackageName))
	{
		UE_LOG(LogChanneldRepGenerator, Warning, TEXT("Unable to find field 'ProtoPackageName'"));
	}

	return true;
}

bool FReplicatorGeneratorManager::SaveGeneratedManifest(const FGeneratedManifest& Manifest, FString& Message)
{
	EnsureReplicatorGeneratedIntermediateDir();
	return SaveGeneratedManifest(Manifest, GenManager_GeneratedManifestFilePath, Message);
}

bool FReplicatorGeneratorManager::SaveGeneratedManifest(const FGeneratedManifest& Manifest, const FString& Filename, FString& Message)
{
	if (!FPaths::DirectoryExists(FPaths::GetPath(Filename)))
	{
		Message = FString::Printf(TEXT("Unable to find the directory of GeneratedManifest: %s"), *Filename);
		return false;
	}
	FString Json;
	TSharedPtr<FJsonObject> RootObject = TSharedPtr<FJsonObject>();

	const TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&Json);
	JsonWriter->WriteObjectStart();
	JsonWriter->WriteValue(TEXT("GeneratedTime"), Manifest.GeneratedTime.ToUnixTimestamp());
	JsonWriter->WriteValue(TEXT("ProtoPackageName"), Manifest.ProtoPackageName);
	JsonWriter->WriteObjectEnd();
	JsonWriter->Close();
	if (FFileHelper::SaveStringToFile(Json, *Filename))
	{
		Message = FString::Printf(TEXT("Unable to save GeneratedManifest: %s"), *Filename);
		return false;
	}
	return true;
}