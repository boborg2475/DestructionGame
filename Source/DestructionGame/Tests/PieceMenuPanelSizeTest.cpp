// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Uniquely named namespace, not anonymous: a unity build merges translation units. The
 * `using namespace` lives inside RunTest for the same reason.
 */
namespace PieceMenuPanelSizeTestSupport
{
	/**
	 * The full panel, 800 x 560 px, transcribed rather than imported. The width is measured: the
	 * longest joint line overran the old 640 px panel by 101 px, plus 39 px for the longer
	 * `generalpurposemortarperpend`, is 780; 800 leaves about 20 px clearance. The height is about
	 * half a 1080 viewport. Pinned so Full cannot drift while Compact is added.
	 */
	constexpr double FullPanelWidthPx = 800.0;
	constexpr double FullPanelHeightPx = 560.0;

	/**
	 * Compact must keep at most two thirds of the full panel's area, per EPieceMenuDetail::Compact's
	 * own "buys back a third" claim. Area, because "strictly smaller" alone passes at one pixel.
	 */
	constexpr double CompactPanelAreaBudget = 2.0 / 3.0;

	const TCHAR* NameOfDetail(EPieceMenuDetail Detail)
	{
		switch (Detail)
		{
		case EPieceMenuDetail::Full:    return TEXT("Full");
		case EPieceMenuDetail::Compact: return TEXT("Compact");
		}

		return TEXT("<not a detail mode>");
	}

	FString DescribeSize(const FVector2D& SizePx)
	{
		return FString::Printf(TEXT("%.2f x %.2f px"), SizePx.X, SizePx.Y);
	}

	bool SizeIsFinite(const FVector2D& SizePx)
	{
		return FMath::IsFinite(SizePx.X) && FMath::IsFinite(SizePx.Y);
	}

	/** One row: a mode the panel can be asked for, and what to call it in a failure. */
	struct FPanelSizeCase
	{
		const TCHAR* Description;
		EPieceMenuDetail Detail;
	};
}

/**
 * The panel size is the presenter's answer per detail mode; Compact is strictly narrower and
 * shorter than Full, and within the area budget. Previously both modes drew at 800 x 560, so
 * Compact freed no screen space.
 *
 * Full is pinned exactly; Compact is only bounded, since its real size must be measured from a
 * laid-out panel (World.Menu.TheReadoutFitsInsideThePanel, which also checks the widget honours
 * this answer). No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPanelSizeTest,
	"DestructionGame.Presenter.PieceMenuPanelSize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPanelSizeTest::RunTest(const FString& Parameters)
{
	using namespace PieceMenuPanelSizeTestSupport;

	const FVector2D Full = PieceMenuPanelSizePx(EPieceMenuDetail::Full);
	const FVector2D Compact = PieceMenuPanelSizePx(EPieceMenuDetail::Compact);

	// Undeclared enumerator: must fail closed to Full, since Compact would hide numbers.
	const EPieceMenuDetail UnknownDetail = static_cast<EPieceMenuDetail>(200);

	const TArray<FPanelSizeCase> Cases = {
		{ TEXT("the full panel"), EPieceMenuDetail::Full },
		{ TEXT("the compact panel"), EPieceMenuDetail::Compact },
		{ TEXT("a detail mode this build has never heard of"), UnknownDetail },
	};

	for (const FPanelSizeCase& Case : Cases)
	{
		const FVector2D SizePx = PieceMenuPanelSizePx(Case.Detail);

		// Finiteness checked separately, so NaN is reported as NaN.
		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): the panel's size must be a number, it is %s"),
				Case.Description, NameOfDetail(Case.Detail), *DescribeSize(SizePx)),
			SizeIsFinite(SizePx));

		// A zero-size panel has no heading to drag.
		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): the panel must have a real size on both axes, it is %s"),
				Case.Description, NameOfDetail(Case.Detail), *DescribeSize(SizePx)),
			SizePx.X > 0.0 && SizePx.Y > 0.0);

		// No mode is larger than Full, which shows the most.
		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): no mode may be larger than the full panel (%s), it asks for %s"),
				Case.Description, NameOfDetail(Case.Detail), *DescribeSize(Full), *DescribeSize(SizePx)),
			SizePx.X <= Full.X && SizePx.Y <= Full.Y);

		// Deterministic: read at build time and again at placement.
		TestEqual(
			FString::Printf(
				TEXT("%s (%s): asking twice must give one answer, %s became %s"),
				Case.Description, NameOfDetail(Case.Detail),
				*DescribeSize(SizePx), *DescribeSize(PieceMenuPanelSizePx(Case.Detail))),
			PieceMenuPanelSizePx(Case.Detail), SizePx);
	}

	// Full stays at its measured size.
	TestEqual(
		FString::Printf(
			TEXT("the full panel must stay the measured 800 x 560 px, it is %s"), *DescribeSize(Full)),
		Full, FVector2D(FullPanelWidthPx, FullPanelHeightPx));

	// An unknown mode falls back to Full.
	TestEqual(
		FString::Printf(
			TEXT("a detail mode this build has never heard of must fall back to the full size (%s), it answers %s"),
			*DescribeSize(Full), *DescribeSize(PieceMenuPanelSizePx(UnknownDetail))),
		PieceMenuPanelSizePx(UnknownDetail), Full);

	// Compact is smaller on each axis, asserted separately.
	TestTrue(
		*FString::Printf(
			TEXT("the compact panel must be strictly NARROWER than the full one: %.2f px against %.2f px, giving back %.2f px"),
			Compact.X, Full.X, Full.X - Compact.X),
		Compact.X < Full.X);

	TestTrue(
		*FString::Printf(
			TEXT("the compact panel must be strictly SHORTER than the full one: %.2f px against %.2f px, giving back %.2f px"),
			Compact.Y, Full.Y, Full.Y - Compact.Y),
		Compact.Y < Full.Y);

	// And by enough to matter (CompactPanelAreaBudget); the percentage is logged for retuning.
	const double FullAreaSqPx = Full.X * Full.Y;
	const double CompactAreaSqPx = Compact.X * Compact.Y;

	AddInfo(FString::Printf(
		TEXT("full %s = %.0f sq px; compact %s = %.0f sq px, which is %.1f %% of it"),
		*DescribeSize(Full), FullAreaSqPx, *DescribeSize(Compact), CompactAreaSqPx,
		FullAreaSqPx > 0.0 ? 100.0 * CompactAreaSqPx / FullAreaSqPx : 0.0));

	TestTrue(
		*FString::Printf(
			TEXT("the compact panel must buy back a third of the panel's area: %.0f sq px against a budget of %.0f sq px (%.1f %% of the full panel's %.0f sq px)"),
			CompactAreaSqPx, CompactPanelAreaBudget * FullAreaSqPx,
			FullAreaSqPx > 0.0 ? 100.0 * CompactAreaSqPx / FullAreaSqPx : 0.0, FullAreaSqPx),
		CompactAreaSqPx <= CompactPanelAreaBudget * FullAreaSqPx);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
