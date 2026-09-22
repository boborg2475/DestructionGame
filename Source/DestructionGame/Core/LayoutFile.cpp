// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/LayoutFile.h"

#include "Core/ContactSweep.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/*
 * File-local names carry a LayoutFile prefix and sit in a named namespace: a unity build merges
 * files, so two anonymous same-named symbols would collide as one hard compile error.
 */
namespace DestructionLayoutFile
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	const TCHAR* const FormatName = TEXT("DestructionGame.Layout");

	FString ContentPath(const TCHAR* Name)
	{
		return FPaths::ProjectContentDir() / TEXT("Layouts") / (FString(Name) + TEXT(".json"));
	}

	/** The library row with this name, or null. */
	static const FMaterialProfile* LayoutFileMaterialNamed(const FString& Name)
	{
		for (const FNamedMaterialProfile& Row : AllMaterialProfiles())
		{
			if (Name.Equals(Row.Name, ESearchCase::CaseSensitive))
			{
				return &Row.Profile;
			}
		}

		return nullptr;
	}

	/** The library name of this profile, or null for a profile the library does not hold. */
	static const TCHAR* LayoutFileNameOfMaterial(const FMaterialProfile* Material)
	{
		for (const FNamedMaterialProfile& Row : AllMaterialProfiles())
		{
			if (&Row.Profile == Material)
			{
				return Row.Name;
			}
		}

		return nullptr;
	}

	/** Three finite numbers out of a JSON array, or false. */
	static bool LayoutFileReadVector(const TSharedPtr<FJsonObject>& Piece, const TCHAR* Field, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;

		if (!Piece->TryGetArrayField(Field, Values) || Values == nullptr || Values->Num() != 3)
		{
			return false;
		}

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			double Value = 0.0;

			if (!(*Values)[Axis].IsValid() || !(*Values)[Axis]->TryGetNumber(Value) || !FMath::IsFinite(Value))
			{
				return false;
			}

			Out[Axis] = Value;
		}

		return true;
	}

	static bool LayoutFileRefuse(FString* OutError, const FString& Why)
	{
		if (OutError != nullptr)
		{
			*OutError = Why;
		}

		return false;
	}

	bool Parse(const FString& Json, FBrickLayout& OutLayout, FString* OutError)
	{
		OutLayout = FBrickLayout();

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);

		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return LayoutFileRefuse(OutError, TEXT("not a JSON object"));
		}

		FString Format;

		if (!Root->TryGetStringField(TEXT("format"), Format) || Format != FormatName)
		{
			return LayoutFileRefuse(OutError, FString::Printf(TEXT("format is not '%s'"), FormatName));
		}

		int32 Version = 0;

		if (!Root->TryGetNumberField(TEXT("version"), Version) || Version != FormatVersion)
		{
			return LayoutFileRefuse(OutError, FString::Printf(TEXT("version is not %d"), FormatVersion));
		}

		double JointThicknessCm = 0.0;

		if (!Root->TryGetNumberField(TEXT("jointThicknessCm"), JointThicknessCm) || !(JointThicknessCm > 0.0))
		{
			return LayoutFileRefuse(OutError, TEXT("jointThicknessCm is missing or not positive"));
		}

		bool bThreeDimensional = false;
		Root->TryGetBoolField(TEXT("threeDimensional"), bThreeDimensional);

		const TArray<TSharedPtr<FJsonValue>>* Pieces = nullptr;

		if (!Root->TryGetArrayField(TEXT("pieces"), Pieces) || Pieces == nullptr || Pieces->Num() == 0)
		{
			return LayoutFileRefuse(OutError, TEXT("no pieces"));
		}

		FBrickLayout Laid;

		for (int32 Index = 0; Index < Pieces->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* PieceObject = nullptr;

			if (!(*Pieces)[Index].IsValid() || !(*Pieces)[Index]->TryGetObject(PieceObject) || PieceObject == nullptr)
			{
				return LayoutFileRefuse(OutError, FString::Printf(TEXT("piece %d is not an object"), Index));
			}

			FVector MinCm;
			FVector MaxCm;

			if (!LayoutFileReadVector(*PieceObject, TEXT("min"), MinCm)
				|| !LayoutFileReadVector(*PieceObject, TEXT("max"), MaxCm))
			{
				return LayoutFileRefuse(OutError, FString::Printf(TEXT("piece %d has no min/max box"), Index));
			}

			if (!(MaxCm.X > MinCm.X) || !(MaxCm.Y > MinCm.Y) || !(MaxCm.Z > MinCm.Z))
			{
				return LayoutFileRefuse(OutError, FString::Printf(TEXT("piece %d has a degenerate box"), Index));
			}

			FString MaterialName;
			const FMaterialProfile* Material =
				(*PieceObject)->TryGetStringField(TEXT("material"), MaterialName)
					? LayoutFileMaterialNamed(MaterialName)
					: nullptr;

			if (Material == nullptr)
			{
				return LayoutFileRefuse(
					OutError, FString::Printf(TEXT("piece %d names no known material ('%s')"), Index, *MaterialName));
			}

			bool bGrounded = false;
			(*PieceObject)->TryGetBoolField(TEXT("grounded"), bGrounded);

			FPieceBox Box;
			Box.CentreCm = (MinCm + MaxCm) / 2.0;
			Box.ExtentCm = (MaxCm - MinCm) / 2.0;

			const int32 Piece = Laid.Structure.AddPiece(
				PieceMassKg(Box, Material->DensityGramsPerCubicCm), bGrounded, Box.CentreCm);

			if (Piece == INDEX_NONE)
			{
				return LayoutFileRefuse(OutError, FString::Printf(TEXT("piece %d was refused by the structure"), Index));
			}

			Laid.Boxes.Add(Box);
			Laid.Structure.SetPieceMaterial(Piece, Material);
		}

		if (!SweepContacts(Laid, JointThicknessCm))
		{
			return LayoutFileRefuse(OutError, TEXT("the contact sweep refused a joint"));
		}

		Laid.Structure.SetThreeDimensional(bThreeDimensional);

		OutLayout = MoveTemp(Laid);

		return true;
	}

	FString Serialize(const FBrickLayout& Layout, double JointThicknessCm)
	{
		/*
		 * Written by hand, not via FJsonSerializer, so the file is one piece per line: a building of
		 * thousands of pieces diffs line by line instead of as one giant line.
		 */
		FString Out;
		Out.Reserve(Layout.Boxes.Num() * 96 + 256);

		Out += FString::Printf(
			TEXT("{\n  \"format\": \"%s\",\n  \"version\": %d,\n  \"jointThicknessCm\": %s,\n  \"threeDimensional\": %s,\n  \"pieces\": [\n"),
			FormatName, FormatVersion, *FString::SanitizeFloat(JointThicknessCm),
			Layout.Structure.IsThreeDimensional() ? TEXT("true") : TEXT("false"));

		const int32 NumPieces = FMath::Min(Layout.Boxes.Num(), Layout.Structure.NumPieces());

		for (int32 Piece = 0; Piece < NumPieces; ++Piece)
		{
			const FPieceBox& Box = Layout.Boxes[Piece];
			const FVector MinCm = Box.CentreCm - Box.ExtentCm;
			const FVector MaxCm = Box.CentreCm + Box.ExtentCm;
			const TCHAR* MaterialName = LayoutFileNameOfMaterial(Layout.Structure.GetPiece(Piece).Material);

			Out += FString::Printf(
				TEXT("    { \"min\": [%s, %s, %s], \"max\": [%s, %s, %s], \"material\": \"%s\", \"grounded\": %s }%s\n"),
				*FString::SanitizeFloat(MinCm.X), *FString::SanitizeFloat(MinCm.Y), *FString::SanitizeFloat(MinCm.Z),
				*FString::SanitizeFloat(MaxCm.X), *FString::SanitizeFloat(MaxCm.Y), *FString::SanitizeFloat(MaxCm.Z),
				MaterialName != nullptr ? MaterialName : TEXT(""),
				Layout.Structure.GetPiece(Piece).bIsGrounded ? TEXT("true") : TEXT("false"),
				Piece + 1 < NumPieces ? TEXT(",") : TEXT(""));
		}

		Out += TEXT("  ]\n}\n");

		return Out;
	}

	bool LoadFile(const FString& Path, FBrickLayout& OutLayout, FString* OutError)
	{
		OutLayout = FBrickLayout();

		FString Json;

		if (!FFileHelper::LoadFileToString(Json, *Path))
		{
			return LayoutFileRefuse(OutError, FString::Printf(TEXT("could not read '%s'"), *Path));
		}

		return Parse(Json, OutLayout, OutError);
	}

	bool SaveFile(const FString& Path, const FBrickLayout& Layout, double JointThicknessCm)
	{
		return FFileHelper::SaveStringToFile(
			Serialize(Layout, JointThicknessCm), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}
