// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace, and named differently from every other one in this module — an anonymous
 * namespace is private to a translation unit, not a file, and a unity build merges many files
 * into one. `using namespace` lives inside RunTest for the same reason.
 */
namespace PieceMenuPanelSizeTestSupport
{
	/**
	 * The panel as it is drawn today, transcribed rather than imported: 800 x 560 px.
	 *
	 * PieceMenuPanelWidthPx and PieceMenuPanelHeightPx are file-static in
	 * DestructionGamePlayerController.cpp and a test may not reach them — the right way round,
	 * since the point of this test is that the size becomes the PRESENTER's answer, so the mode
	 * can change it and something can read what it said.
	 *
	 * Neither number is picked, and the width is the one with a derivation. 800 px is a measured
	 * floor plus a stated clearance: World.Menu.TheReadoutFitsInsideThePanel lays the readout over
	 * a ragged wall whose corbel sits on one off-centre patch — the only shape whose joint line
	 * grows a bending clause — and the measurement moved when the joint row learned to name the
	 * profile that fastens it: that line overran the old 640 px panel by 101 px carrying
	 * `generalpurposemortar`, plus a 39 px floor for the seven-character-longer
	 * `generalpurposemortarperpend`. 640 + 101 + 39 = 780, and 800 leaves 59 px of spare (the
	 * perpend's 39 plus ~20 px of real clearance). The height is a fit, not a measurement: 560 px
	 * is about half a 1080 viewport, so the list, the readout and the action rows are all on
	 * screen at once.
	 *
	 * Pinning them here is what keeps *Full* honest while Compact is added: a mode-dependent size
	 * whose Full arm quietly drifted would take the measured floor with it, and the wall that can
	 * see that floor is expensive to build, so the cheap claim lives here and the expensive one
	 * stays where the pixels are.
	 */
	constexpr double FullPanelWidthPx = 800.0;
	constexpr double FullPanelHeightPx = 560.0;

	/**
	 * How much of the panel Compact has to give back — the model's own claim, not a number
	 * chosen here.
	 *
	 * EPieceMenuDetail::Compact says of itself, in Core/PieceMenu.h, that dropping the joint
	 * table "is the cut that buys back a third of the screen". A third back is two thirds left,
	 * read as a fraction of the PANEL — the conservative reading, and the only one this function
	 * can promise.
	 *
	 * Area rather than either axis, because "strictly smaller" is satisfied by one pixel: a
	 * 639 x 559 px compact panel is smaller on both axes, passes every ordering claim below, and
	 * answers the player's complaint — "it takes up so much of the screen" — with nothing at all.
	 * The per-axis claims say which way it must shrink; this one says it has to be worth doing.
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
 * A compact panel is a smaller panel — strictly narrower *and* strictly shorter than the full
 * one — and how big either of them is, is the presenter's answer rather than a constant beside
 * the slate that draws it.
 *
 * The complaint this is the second half of, verbatim: "I can't move the menu if it is in my way
 * and it takes up so much of the screen." The first half is answered — ClampPanelOffset is in
 * and the title strip drags. The second is not: EPieceMenuDetail::Compact drops the joint table
 * and the headroom scale, then draws what's left into a rectangle overridden to exactly
 * 800 x 560 px whatever the mode, with the readout in a slot that fills whatever it is given. So
 * a compact panel is a full-sized panel with fewer words in it, and the player gets back no
 * screen at all. Making the size a function of the mode is the behaviour; making that function
 * live here is what makes it assertable.
 *
 * THE ASSERTIONS ARE ORDERINGS AND A BUDGET, NOT A COMPACT SIZE. This test deliberately does not
 * say what the compact panel measures, because that figure has to be DERIVED the way the full
 * one was — from the longest line the mode still shows, an entry row rather than a joint row,
 * which only a laid-out panel over a real wall can measure. That's
 * World.Menu.TheReadoutFitsInsideThePanel's job, and it now sweeps both modes. Pinned here is
 * everything that measurement cannot say: the answer exists, is finite and positive, Full has
 * not drifted off its own measured floor, Compact is smaller on BOTH axes rather than one, and
 * by enough to be worth a player's while.
 *
 * FULL IS PINNED EXACTLY AND COMPACT IS BOUNDED — the asymmetry the derivation implies. 800 x 560
 * is a measured floor plus a stated clearance, and a retune of it should be a decision made on
 * purpose; the compact figure is a derivation this test cannot perform, so pinning one here would
 * invent the number the task exists to avoid inventing.
 *
 * WHAT THIS CANNOT REACH: whether the widget honours the answer. An SBox override sets a desired
 * size and a filling slot arranges its child at whatever width it was handed — this project has
 * already shipped a 540 px canvas sitting over 96 px bars on exactly that mistake. That join is
 * measured in World.Menu.TheReadoutFitsInsideThePanel, which arranges the real panel in both
 * modes and compares what moved against what this function said.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. One enumerator in, two doubles out.
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

	/*
	 * An enumerator nobody declared — the fail-closed row. EPieceMenuDetail is a uint8 and a
	 * cast is all it takes to produce one; what matters is which arm an unknown value falls
	 * into. Compact would SUPPRESS numbers somebody asked for, which is why the enum's own
	 * comment makes Full enumerator zero.
	 */
	const EPieceMenuDetail UnknownDetail = static_cast<EPieceMenuDetail>(200);

	const TArray<FPanelSizeCase> Cases = {
		{ TEXT("the full panel"), EPieceMenuDetail::Full },
		{ TEXT("the compact panel"), EPieceMenuDetail::Compact },
		{ TEXT("a detail mode this build has never heard of"), UnknownDetail },
	};

	for (const FPanelSizeCase& Case : Cases)
	{
		const FVector2D SizePx = PieceMenuPanelSizePx(Case.Detail);

		/*
		 * Finite first and separately from the value: a NaN compares false against everything
		 * including itself, so the positivity claim below would report "it is nan x nan px" as
		 * an ordinary too-small panel rather than the fault it is.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): the panel's size must be a number, it is %s"),
				Case.Description, NameOfDetail(Case.Detail), *DescribeSize(SizePx)),
			SizeIsFinite(SizePx));

		/*
		 * A panel of no size is not a small panel: zero draws no heading, and the heading is the
		 * strip the drag affordance hangs off — a panel that cannot be grabbed is the state
		 * ClampPanelOffset exists to refuse, arrived at from the other direction.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): the panel must have a real size on both axes, it is %s"),
				Case.Description, NameOfDetail(Case.Detail), *DescribeSize(SizePx)),
			SizePx.X > 0.0 && SizePx.Y > 0.0);

		/*
		 * No mode is bigger than Full on either axis: Full withholds nothing, so it needs the
		 * most room, and a mode asking for more is asking for more room to show less.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): no mode may be larger than the full panel (%s), it asks for %s"),
				Case.Description, NameOfDetail(Case.Detail), *DescribeSize(Full), *DescribeSize(SizePx)),
			SizePx.X <= Full.X && SizePx.Y <= Full.Y);

		/*
		 * And the answer is the same answer twice: read while a widget tree is built and again
		 * while the panel is placed. A size that moved between calls would be a panel anchored to
		 * a corner it is not the size of.
		 */
		TestEqual(
			FString::Printf(
				TEXT("%s (%s): asking twice must give one answer, %s became %s"),
				Case.Description, NameOfDetail(Case.Detail),
				*DescribeSize(SizePx), *DescribeSize(PieceMenuPanelSizePx(Case.Detail))),
			PieceMenuPanelSizePx(Case.Detail), SizePx);
	}

	/*
	 * The full panel is the size the game already draws, to the pixel — the characterisation
	 * half: the behaviour being added is Compact, and Full moving with it would silently retune a
	 * width measured against the longest sentence this readout can compose.
	 */
	TestEqual(
		FString::Printf(
			TEXT("the full panel must stay the measured 800 x 560 px, it is %s"), *DescribeSize(Full)),
		Full, FVector2D(FullPanelWidthPx, FullPanelHeightPx));

	/* And the mode nobody declared answers as Full does, rather than hiding half the readout. */
	TestEqual(
		FString::Printf(
			TEXT("a detail mode this build has never heard of must fall back to the full size (%s), it answers %s"),
			*DescribeSize(Full), *DescribeSize(PieceMenuPanelSizePx(UnknownDetail))),
		PieceMenuPanelSizePx(UnknownDetail), Full);

	/*
	 * The two claims the whole file is for, one per axis and separately, because the two failures
	 * look different on screen: a panel that only narrowed leaves a full-height ribbon down the
	 * side of the viewport; one that only shortened leaves a full-width band across it. Either is
	 * a mode that dropped the joint table and kept the room it was taking.
	 */
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

	/*
	 * And it must give back enough to be worth asking for (see CompactPanelAreaBudget): the
	 * model's own description is that it buys back a third, and "strictly smaller" alone is
	 * satisfied by a pixel. Reported as a percentage, since a retune needs how much room is
	 * left, not whether it passed.
	 */
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
