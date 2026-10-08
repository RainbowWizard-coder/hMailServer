# Control Panel design system

The desktop Control Panel (`hmailserver/source/Tools/ControlPanel`, WPF on .NET 10 with WPF-UI 4.3) is being redesigned to read as one administration console - calm, dense enough for an administrator, the same on every page, and right in the light theme, the dark theme and High Contrast. This document is the contract the page waves build against: it names every token and every component, with its exact name and its public surface, and it says what the shell does around a page. It was written on 15 September 2026 with the first wave, which delivered the frame and migrated two pages - the sign-in page and the dashboard - as the pattern, and the other three waves followed it the same day: every page, both long settings views and all eighteen dialogs are on it now; a test (`ControlPanel.Tests/Services/DesignTokensTests.cs`) holds this document to the tokens file and the components, so a token or a component that is not here does not build.

Three files carry the design. `Services/DesignTokens.cs` holds the numbers and the colours as plain C#, with no WPF, so the tests can hold them; `Views/Scaffold/Tokens.xaml` declares the same values as resources for XAML; `Views/Scaffold/Scaffold.xaml` holds the components' templates, as implicit styles keyed by type, and merges the tokens. `App.xaml` merges `Scaffold.xaml` after WPF-UI's own dictionaries. `Services/ThemeTokens.cs` turns the colours into brushes on every theme change, and `Services/Typography.cs` still holds the type ramp the sizes below repeat. The three status colours themselves stay in `Services/StatusPalette.cs`, where `ColourVisionTests` holds every pair apart for colour-blind eyes.

## The frame

`MainWindow.xaml` is a WPF-UI `FluentWindow` with the Mica backdrop, in three rows: the title bar, the command bar, then the sidebar beside the content.

The **command bar** (`command-bar`) is `AppCommandBarHeight` tall and holds, left to right: the sidebar toggle (`sidebar-toggle`, its tool tip and accessible name saying what a click will do), the brand, the search (`nav-search` - a button drawn as a field, because what it opens is the Ctrl+K palette and that is where the typing happens; the shortcut still works), the server status pill (`server-status`, a `StatusPill` fed by `ServerSession.LinkStateChanged` and by sign-in: *Not connected* while the sign-in page shows, *Connected*, *Reconnecting…* while the session heals, *Connection lost* when it cannot; its accessible name is "Connected to host as user"), the theme toggle (`theme-toggle`) and the administrator's menu (`admin-menu`, showing the account's name once signed in and holding who is signed in where, the server version, the language, the theme and the About page). Nothing that was in the sidebar's footer is lost: the connection badge became the pill, the language and theme buttons moved into the bar.

The **sidebar** is the `TreeView` built from `Services/NavigationMap.cs`, in the order a new administrator needs it (since 27 September 2026): *Welcome*, *Dashboard*, then *Set up this server* - the *Setup checklist*, *Setting profiles* and *DNS records* - then *Accounts & domains*, *Mail flow & delivery*, *Connections & protocols*, *Certificates & encryption*, *Spam & virus filtering*, *Sign-in & access*, *Monitoring & troubleshooting*, *Maintenance* and *About*. Every page automation id (`nav-<key>`) is what it was; one page title changed (*External setup* is the *Setup checklist*), and two group headings were put into plain words (*TLS & certificates*, *Access & abuse protection*); each old name is an alias the palette finds, and a group's `navgroup-<slug>` follows its heading. A page row ellipsizes its title when the pane is narrow, as a group row does, and the whole title is in its tool tip. Rows are `AppNavRowHeight` tall with the control radius, the hover fill and the brand pill on the selected row; pages are indented under their group's label rather than under its glyph, so the two text columns line up. Below `DesignTokens.Shell.CollapseSidebarBelow` (1000 pixels of window width) the sidebar collapses to the **rail** (`nav-rail`): one glyph per top-level group (`navrail-<slug>`), the group of the page on screen marked, a click expanding the sidebar on that group. The toggle collapses and expands it by choice above that width, and the choice is kept in the registry beside the window bounds (`SidebarCollapsed` under `HKCU\Software\hMailServer\ControlPanel`). The footer under the tree carries the server's version and hides with the rail.

The **content area** keeps the breadcrumb bar (`breadcrumb-bar`, the trail and the *Related:* signposts) above the page. A page owns its own `ScrollViewer` and pads itself with `AppPagePadding`; the host adds nothing. The window remembers its size, position and maximised state as before.

A page that edits says whether it holds **unsaved changes** - `IPageLifecycle.HasUnsavedChanges`, with `SaveChanges()` to save them as its own Save does - because the window keeps a page once it is built and the page reads the server again as it is next opened. The window asks before anything replaces such a page: the sidebar's selection, which every link, the palette and a tour go through, closing the window and a change of language. *Save* goes on only when everything was saved, *Discard* goes on, and *Stay* - Escape's and Enter's answer - keeps the page and puts the sidebar back on it. A page's own Reload asks *Discard your unsaved changes and read the settings again?* before it reads the server. `Services/UnsavedChanges.cs` holds the decision and `EditorBaseline`, the record of what a hand-built page's editors held when they were last read or saved; `UnsavedChangesTests` holds both and the wiring, including that every page with a *Save changes* button says whether it holds an unsaved change.

## Tokens

The numbers first. Every margin, padding and gap in a page is one of the six steps; a layout that needs a seventh has gone wrong. In C# the same values are `DesignTokens.Space.Xs` … `Xxl`, `DesignTokens.Radius.Control` / `Card` / `Pill`, `DesignTokens.Shell.CommandBarHeight` / `RailWidth` / `NavRowHeight` / `CollapseSidebarBelow` / `SidebarMinWidth` / `SidebarMaxWidth`, and `DesignTokens.Dialog.MinWidth` / `DefaultWidth` / `MaxWidth` / `MaxHeightFraction` (360, 520, 900, 0.9).

| Token | Value | What it is for |
|---|---|---|
| `AppSpaceXs` | 4 | The gap between a mark and its word, a label and its editor. |
| `AppSpaceSm` | 8 | The gap between buttons in a row, between a pill and a title. |
| `AppSpaceMd` | 12 | The gap between cards in a row, under a toolbar, between the rows of a form. |
| `AppSpaceLg` | 16 | Under a page header, the inset of a compact card. |
| `AppSpaceXl` | 24 | The page's side padding, between the sections of a settings page, a dialog's inset. |
| `AppSpaceXxl` | 32 | The inset of the sign-in card and of an empty or loading state. |
| `AppPagePadding` | 24,20,24,24 | The padding of a page's `ScrollViewer`. Every page uses it; a page with a padding of its own is a page that will look wrong beside the others. |
| `AppCardPadding` | 20,16 | A card's inset - the `Card` component's default and the old `Card` Border style's. |
| `AppHeaderGap` | 0,0,0,16 | The margin under a `PageHeader`. |
| `AppCardGap` | 0,0,0,12 | The margin under a card, a toolbar or a notice that another follows. |
| `AppFieldGap` | 0,0,0,12 | The margin under a `FieldRow`. |
| `AppSectionGap` | 0,0,0,24 | The margin under a `SettingsSection`. |
| `AppControlCornerRadius` | 4 | Controls, navigation rows, notices. |
| `AppCardCornerRadius` | 8 | Cards and dialogs. |
| `AppPillCornerRadius` | 4 | The status pill's box: the control radius, so it reads as a filled box and not a capsule. |
| `AppCommandBarHeight` | 48 | The command bar. |
| `AppFormWidth` | 760 | The widest a form is laid out, whatever the window does: a caption and its editor at opposite ends of a maximised window are unreadable. `DesignTokens.FormWidth`. |
| `AppRailWidth` | 48 | The sidebar collapsed to icons. |
| `AppNavRowHeight` | 34 | One navigation row, and the minimum height of a grid row. |

## Type

The ramp is Windows 11's: Caption 12, Body 14 (Body Strong is 14 SemiBold), Subtitle 20, Title 28. The sizes are resources and the roles are keyed `TextBlock` styles. They are keyed on purpose and must stay so: an implicit TextBlock style reaches into every control template and overrides the on-accent button text, which is how the forum's one look complaint arose (the note in `App.xaml` tells the story). The default text colour comes from property inheritance - the window and `FluentDialogWindow` set their Foreground to the theme's primary text brush - so a bare TextBlock needs no style to be readable.

| Token | Value | What it is for |
|---|---|---|
| `AppFontSizeCaption` | 12 | Captions, hints, field labels, the breadcrumb, the pill. |
| `AppFontSizeBody` | 14 | Body text, controls, navigation rows, card titles. |
| `AppFontSizeSubtitle` | 20 | A dialog's heading, the sign-in card's heading. |
| `AppFontSizeTitle` | 28 | The page title. |
| `TextTitle` | 28 SemiBold, primary, trimmed with an ellipsis | The page title; the `PageHeader` draws it. The older `PageTitle` style stays for the unmigrated pages and adds the level-1 heading and a margin. |
| `TextSubtitle` | 20 SemiBold, primary, wraps | A dialog's or a card-page's heading. |
| `TextBodyStrong` | 14 SemiBold, primary, wraps | Card titles, section headings, the summary line of a notice card. |
| `TextBody` | 14, primary, wraps | Body text. |
| `TextSecondary` | 14, secondary, wraps | The page subtitle, an empty state's sentence. |
| `TextCaption` | 12, secondary, wraps | Hints, descriptions, field labels, notes. |
| `TextCaptionTertiary` | 12, tertiary, wraps | The quietest text: a keyboard hint, a footer line. |

Two rules the shared templates in `App.xaml` keep, because each was broken once and reached every page. The tab strip weights and sizes its headers alone: the selected tab's SemiBold and the strip's 14 px are set on the header's presenter in the `TabItem` template (`TextElement.FontWeight`, `TextElement.FontSize`), never on the `TabItem`, since both inherit and a tab's page is the `TabItem`'s logical child - a setter on the item made every caption, hint and editor value on the open tab as heavy as its card titles. And a check box or radio button whose `Content` is a string shows it in an `AccessText` that wraps (an implicit data template for strings in the two implicit styles), so a long caption folds under itself instead of being cut mid-word at the edge of its card or dialog, keeps its Alt key, and is still the box's accessible name; a box whose `Content` is an element of its own is presented as before. Both implicit styles are based on WPF-UI's under its own key (`DefaultCheckBoxStyle`, `DefaultRadioButtonStyle`), never on `{StaticResource {x:Type CheckBox}}`: inside the dictionary that defines an implicit style, a StaticResource for the type key finds the entry being defined and yields null, and a style based on nothing gives every box the classic theme's control. A caption that needs more than a line or two is better short, with the consequence in a note under it that is also the box's help text (`AutomationProperties.SetHelpText`), so a screen reader reading the box still hears it. `SharedTemplatesTests` holds the templates and `ConsequenceNoteTests` the help text.

## Colour

Five status colours and a neutral, as brushes that `ThemeTokens.Refresh` recomputes on every theme change and republishes on the application's own dictionary, which shadows the dark values `Tokens.xaml` declares. Reference them with `DynamicResource`, never `StaticResource`, and in code with `SetResourceReference` by key (`StatusSemantics.For(level).BrushKey` gives the key for a level) rather than by holding a brush - a resolved brush keeps the colour of the theme it was resolved under. The light values each clear 4.5:1 on white; the dark values clear it on #1B1B1B. Under High Contrast every colour comes from the system palette and none is invented: brand and information are the highlight, the three status colours are the window text - their shape and word tell them apart, which is the rule below - neutral is the disabled-text grey, a card is the window colour with a window-text outline, and the notice tint goes to zero.

| Token | Light | Dark | High Contrast | What it is for |
|---|---|---|---|---|
| `AppBrandBrush` | #2F6FE0 | #4C8DFF | Highlight | The selection pill, the accent that is not a status. |
| `AppSuccessBrush` | #1A7F37 | #3FB950 | WindowText | Good: connected, done, passed. Square. |
| `AppWarningBrush` | #9A6700 | #D29922 | WindowText | Warning: a backlog, a certificate near expiry, a reconnect in progress. Triangle. |
| `AppDangerBrush` | #CF222E | #F85149 | WindowText | Critical: a failure, a lost link, a validation message. Cross. |
| `AppInfoBrush` | #6639BA | #A371F7 | Highlight | Information: a fact worth a notice. Diamond. Held at least ΔE 8 from the three above under protanopia and deuteranopia. |
| `AppNeutralBrush` | #57606A | #9DA7B0 | GrayText | A status that says nothing: not connected, a count of zero. |
| `AppCardBackgroundBrush` | #B3FFFFFF | #0FFFFFFF | Window | The card surface. Translucent, so it sits on Mica and on the page alike. |
| `AppCardBorderBrush` | #0F000000 | #14FFFFFF | WindowText | The card's outline. |
| `AppCardHoverBrush` | #80F9F9F9 | #15FFFFFF | Window | A card that can be pointed at - a tile, a rail item - under the pointer. |
| `AppDividerBrush` | #0F000000 | #14FFFFFF | WindowText | The hairline between a card's content and its footer, under the command bar, around the breadcrumb. |
| `AppNoticeTintOpacity` | 0.12 | 0.12 | 0 | The opacity of the status colour behind an `InlineNotice`. |
| `AppFocusBrush` | #7542E5 | #B89CFF | Highlight | Where the keyboard is: the ring round a text field that has it, and the navigation's border round the row it is on. 3:1 or more beside every surface and beside its glow. |
| `AppFocusGlowBrush` | #407542E5 | #47B89CFF | none | The glow outside that ring, at its strongest; it fades over six pixels. |
| `AppSelectionFillBrush` | #1F2F6FE0 | #1F4C8DFF | none | Behind the selected row of every grid, list box and list view: the brand at the notice tint's opacity, so near the card's own that the row's text stays what it was. Beside it the brand pill at the row's left edge. |
| `AppSelectionOutlineBrush` | none | none | Highlight | The one-pixel outline round a selected row under High Contrast, where the fill is nothing and the pill is the highlight too. |
| `AppSuccessFillBrush` | #CCF0D6 | #1C4128 | Window | The fill of a Good `StatusPill` - "Connected", "Done". |
| `AppSuccessFillTextBrush` | #116329 | #56D364 | WindowText | The word and mark on that fill, and a Good notice's word and mark. |
| `AppInfoFillBrush` | #EBE0FF | #3A2A63 | Window | The fill of an Information pill - "Next step". |
| `AppInfoFillTextBrush` | #5A32A3 | #C8A2FF | Highlight | The word and mark on it, and an Information notice's. |
| `AppWarningFillBrush` | #FBE7AE | #45350F | Window | The fill of a Warning pill - "Action needed", "Reconnecting…". |
| `AppWarningFillTextBrush` | #7D4E00 | #E3B341 | WindowText | The word and mark on it, and a Warning notice's. |
| `AppDangerFillBrush` | #FFDAD6 | #522024 | Window | The fill of a Critical pill - "Connection lost". |
| `AppDangerFillTextBrush` | #A40E26 | #FF9492 | WindowText | The word and mark on it, a Critical notice's, and a destructive button's words (`DangerTextButton`). |
| `AppNeutralFillBrush` | #DFE4E9 | #3A4048 | Window | The fill of a Normal pill - "Not connected", a count, "Editing". |
| `AppNeutralFillTextBrush` | #424A53 | #D1D9E0 | GrayText | The word and mark on it. |
| `AppStatusFillBorderThickness` | 0 | 0 | 1 | The pill's outline: none where the fill is the box, one pixel in the text colour under High Contrast, whose fill is the window colour. |

The fill-text shades are not the status colours: those are measured against the page and lose contrast on a tint - the light green on the old grey capsule measured 4.2:1, and on a notice's tint the light warning word measured 4.0:1 and the dark information word on a card 3.6:1. Every fill-text shade clears 5.5:1 on its fill, on a notice's tint, on the page and on a card, and `DesignTokensTests` holds each pair to 4.5:1. `DesignTokens.StatusFill(level, theme, system)` is the table.

The light theme's **tertiary text** is darker than Wpf.Ui's: `ThemeTokens` sets `TextFillColorTertiaryBrush` to black at 58% (`DesignTokens.LightTertiaryText`, #94000000), which composites to #696969 on the page (5.3:1) and #656565 on a hovered row (5.1:1). Wpf.Ui's black at 45% measured 3.3:1, and every field hint, "Applies on save" line and the *Related:* caption is set in it. The dark and High Contrast themes keep the theme's own.

**Focus** is a colour of its own, the violet the Control Deck and the webmail declare as `--focus` and `--focus-glow`: while a text box, password box, number box, combo box or date picker has the keyboard - clicked or tabbed into alike - `FocusGlow` draws a two-pixel ring in `AppFocusBrush` inside its edge, over its border, and a glow outside it that fades over six pixels. It is an adorner, so it keeps the field's size and corner radius and moves nothing, and it is one class handler for every field rather than a template per control. It is drawn on the adorner layer outside any scroll viewer the field is in - a viewer's own layer is clipped to its viewport, and cut the glow off at the sides of every dialog's fields - and clips itself to what of the field is in view, the glow going past only an edge that is (`DesignTokens.FocusGlowBounds`). The theme's own line under a focused field (`TextControlFocusedBorderBrush`, `ComboBoxBorderBrushFocused`) takes the same violet, which is what a field drawn as an underline - the search palette's - shows. It is not drawn under High Contrast, where the theme's focus visuals and colours stand, nor on a disabled field or one with no box - a box is a border on the left, top and right, the bottom being free to be the theme's accent line, as Wpf.Ui's date picker's is; a read-only field takes the keyboard and is ringed; a field whose binding reports a validation error keeps the danger colour for its ring, with the violet glow outside it. The ring fades in over 120 ms unless Windows has animations off. `DesignTokens.Focus(theme, system)` is the table, and `DesignTokensTests` holds the ring to 3:1 beside every surface and its glow, and the web pages' copies to its values.

**Selection** is one language across the application's grids and lists: the selected row of every `DataGrid`, every `ListBox` and every `ListView` - the Ctrl+K palette's results, the public folders, the dialogs' member lists, the four lists on the domains page - takes `AppSelectionFillBrush` behind it and the sidebar's 3 x 16 `AppBrandBrush` pill at its left edge, and keeps its own text: the fill is the brand at the notice tint's opacity, so near the card that the primary and secondary text clear 4.5:1 on it, which `DesignTokensTests` holds them to. The sidebar and the rail keep the theme's subtle grey behind the row they mark with the pill - the Fluent navigation convention, where the pill alone tells that row from a hovered one - so what the lists share with the shell is the pill; the tint is the lists' own, and `SelectionLanguageTests` holds the shell to its grey so that this paragraph stays true. In a list or a grid, hover is the subtle grey alone, so a hovered row and the selected one never look the same. The keyboard's row, in a list, wears a 1.5-pixel ring in `AppFocusBrush` in place of the stock focus visual; a grid keeps the stock visual on its focused cell. Under High Contrast the fill is nothing and `AppSelectionOutlineBrush` draws a one-pixel outline in the highlight round the row, as the pill is. `DesignTokens.Selection(theme, system)` is the table; the implicit `DataGridRow`, `ListBoxItem` and `ListViewItem` styles in `App.xaml` draw it, and a list that styles its own items (the live log's) is left to itself. A list row's content starts where a grid cell's does, ten pixels in and past the pill, in a `ListView` as in a `ListBox`; a page whose rows carry an inset of their own narrows the row's in its own resources, as the domains page does, never in `App.xaml`, where the domains page's six pixels once put the incoming relays' bare rows three pixels from the pill. Before this, a selected grid row was the hover grey (1.08:1 against its neighbours), the domains page's lists drew WPF-UI's grey with an outline, and the palette and the bare list boxes drew its solid accent band, on which the palette's own text measured 1.9 to 3.5:1.

The rule that goes with the palette: **a status is never carried by colour alone.** `StatusSemantics.For(level)` gives every level a colour key, a fill and fill-text key for a pill, a shape (`ShapeMarkVisuals` draws it) and a word, and the components below draw all three and put the word first in their accessible name. A page that paints a value in a status colour adds the shape and the word itself, as the dashboard's queue badge does.

A list page's **destructive action** - Delete, Remove, Delete selected, and the delete on a row, whether it is the word or the trash glyph - is a secondary button with its words in the danger fill-text colour while it can be pressed and the theme's disabled grey while it cannot: the `DangerTextButton` style in `App.xaml`, or `ButtonStyles.DangerText(button)` for a button built in code. A style and not a local `Foreground`, because a local red outranks the button template's disabled state and left a disabled Delete looking live, and because the danger colour itself is measured against the page: as a local colour on a button's fill it measured 3.3 to 3.5:1 in the dark theme. An inactive domain or account on the domains page dims its glyph alone and says *Inactive* in a Normal `StatusPill` after its name, which takes the secondary text; a whole row at 0.55 made the word the least legible text on the page. The filled `Danger` appearance stays for what is confirmed or cannot be undone: a confirmation's destructive choice, *Start restore*.

## Components

All in `hMailServer.ControlPanel.Views.Scaffold`; import it with `xmlns:scaffold="clr-namespace:hMailServer.ControlPanel.Views.Scaffold"` in XAML or `using hMailServer.ControlPanel.Views.Scaffold;` in code (a file that also imports `Wpf.Ui.Controls` will find `Card` ambiguous; qualify it). Each is a control with a template in `Scaffold.xaml`, so it works in XAML and in code alike; each is never a tab stop itself; an empty string set on a text slot is the same as null and collapses the slot; and each is an element of its own in the automation tree with the name given below - a bare WPF `Control` has no automation peer, and a card built from one would reach a screen reader as loose text.

| Component | What it is | Public surface |
|---|---|---|
| `PageHeader` | The top of every page: the title, an optional subtitle under it, an optional status pill beside the title, the page's help button and the page's actions on the right. It is the page's level-1 heading. | `Title`, `Subtitle` (string); `Status`, `Actions` (object slots); `TourId` (string, null for no help button - the shell sets it as it opens a page, from `TourCatalog`); event `ShowTour`, which bubbles to the shell. Automation: Text, HeadingLevel1, Name = Title, HelpText = Subtitle; the help button is `page-help`. |
| `Card` | The surface a page groups things on: an optional title and description above the content, an optional footer under a hairline, the card tokens for surface, border, radius and padding. Without a title it is the plain surface the old `Card` Border style gives. | `Title`, `Description` (string); `Footer` (object); `Content`; `Padding` (default `AppCardPadding`). Automation: Group, Name = Title. |
| `Toolbar` | The row above a grid: a search box, a slot for filters beside it, a slot for the actions on the right. The actions are given the width that is left, so a `WrapPanel` of actions wraps in a narrow window rather than running off its edge. | `SearchText` (string, two-way), `SearchPlaceholder` (string; also the box's accessible name), `ShowSearch` (bool); `Filters`, `Actions` (object slots); event `SearchTextChanged`; `FocusSearch()`. Automation: ToolBar. |
| `EmptyState` | What a grid shows with nothing in it: a glyph, one sentence saying so and what to do, the action that does it. Shown in the grid's place. | `Icon` (`SymbolRegular`, `Empty` for none), `Text` (string); `Action` (object). Automation: Text, Name = Text. |
| `LoadingState` | An indeterminate ring and a line, centred where the data will be; a polite live region. | `Text` (string, default *Loading…*). Automation: Text, Name = Text. |
| `InlineNotice` | A notice in the flow of a page at one of the status levels: the mark, the severity word (collapsed at `Normal`, where the level says nothing and the word would read as a status that is not one), the sentence, room for an action; the level's colour as a tint behind and an outline around. Polite live region; set `AutomationProperties.LiveSetting="Assertive"` on one that must interrupt. | `Level` (`StatusLevel`, default Information), `Text` (string); `Content` (the action). Automation: Text, Name = "Word: text". |
| `StatusPill` | A status as a filled, rounded box: the level's fill, and its mark and a short word in the fill-text shade; no outline except under High Contrast. The word ellipsizes when the pill is squeezed. | `Level` (`StatusLevel`), `Text` (string). Automation: Text, Name = "Word: text". |
| `FieldRow` | One field of a form: the caption (with its Alt key, wired to the editor through `Mnemonic`), the editor, an optional hint under it, an optional validation message under that, drawn with the danger colour, a cross and the words. The editor takes its accessible name from the caption and its help text from the hint and the message. | `Label`, `Hint`, `Error` (string; null clears the message); `Content` (the editor). Automation: Group, Name = the caption. |
| `SettingsSection` | A section of a settings page: a heading, a description, the fields. A level-2 heading under the page's level-1. Named SettingsSection because System.Windows.Documents has a Section and a dozen views import that namespace. | `Heading`, `Description` (string); `Content`. Automation: Group, HeadingLevel2, Name = Heading. |
| `TourOverlay` | The ring a tour draws round the control a step points at, and the card beside it with the sentence, *Step n of m*, Back, Next (or Finish on the last stop) and Skip. Built in code rather than templated, because it is not laid out in a page but floats over one; it is a `Canvas` with no background, so it is hit-testable only where the card is. | `ShowStep(target, tourName, position, sentence, canGoBack, lastStep)`, `Hide()`, `Reposition()`, `FocusCard()`; `Showing`, `HasFocusWithin`; events `Next`, `Back`, `Skip`. Automation: the card is a polite live region named "tour. position. sentence". |
| `DialogFrame` | The shape of a dialog's content: heading and description, a body that scrolls past the dialog's maximum height, a footer with a note on the left and the primary button before the secondary. | `Heading`, `Description` (string); `Body`, `Footer`, `PrimaryButton`, `SecondaryButton` (object). Automation: Pane, Name = Heading. |

The dialogs' frame is reached through `FluentDialogWindow`, the base class of every dialog: `UseFrame(heading, body, primary, secondary, description = null, width = 520)` builds a `DialogFrame`, makes it the window's content, gives Enter to the primary button and Escape to the secondary (or to closing the dialog when there is none), sizes the window to the content at that width within `DesignTokens.Dialog` (the height capped at 0.9 of the work area, the body scrolling past it) and puts the keyboard in the first field of the body on load. `SizeToContentWithin(width)` and `FocusFirstFieldOnLoad(root)` are its two halves, for a dialog that keeps its own layout. All eighteen dialogs are on the frame since 15 September 2026, and none of them names a width any more: each is sized to its content within `DesignTokens.Dialog`. `Views/Dialogs.cs`, the themed message boxes, and `Views/NavigationPalette.cs` are on it too. Two helpers live in `Views/DialogFields.cs` beside them rather than in the base class, which is where they belong and where they should move: `FitTabs` sizes a tabbed dialog to its tallest tab and `FillTabs` gives every tab one height, both measuring each tab's content on its own - selecting a tab and measuring the `TabControl` reports the tab before it, because a control outside a visual tree reaches its content presenter a pass later, and that under-measurement hid the whole add-row of one dialog's address tab until a render caught it.

`FillPanel` is the one panel in the namespace and has no template: a page's parts one under another, the parts marked `scaffold:FillPanel.Fill="True"` - the page's lists, or the cards that hold them - taking the height the others leave in the window, down to their floors, below which the panel is taller than the window and the page scrolls. Every other part is given the height it asks for, so nothing on the page is ever cut off where scrolling cannot reach it. Several filling parts share the room by `scaffold:FillPanel.Weight` (1 unless set), as a grid's star rows share theirs, none of them below its floor. A filling part with a `MinHeight` is floored at it and measured at the height it is given, so that a list in it realises the rows it shows; the IP ranges page's list card is one, at 157 pixels, three rows or the empty state that takes their place. Where layout rounding makes such a part ask for more than it was given - at 150 % scaling a `MinHeight` of 157 is 236 device pixels, 157.33 - it is floored at what it asks for (`PageFill.MeasuredFloor`), so that the parts under it are not drawn a device pixel too high. A filling part without one is floored at the height it asks for when nothing limits it and stretched to its share, which is the shape for a card that holds a toolbar, a list and a form: the card's content is a `FillPanel` of its own whose filling part is the list, with a `MinHeight` of three rows and a pixel for layout rounding, which at 125, 150 or 175 % can take one off (and two more on a `ListView`, whose template keeps one above and one below its rows; with fewer either holds two whole rows) - on a one-part `FillPanel` around the list where an `EmptyState` is laid over it - so that the card's floor is its toolbar, its form and its list's rows, and rises with them when the form wraps or the empty state's sentence needs more than the rows. The outermost `FillPanel` goes inside the page's `ScrollViewer` with `ViewportHeight="{Binding ViewportHeight, ElementName=PageScroll}"`, which is the only way a panel measured inside a scroll viewer learns how high the window is; one inside a filling part needs no binding, since it is given its height when it is arranged. It replaces two shapes that each lost a page something (work item 79): the list in a grid's star row between rows sized to their content, which a window shorter than those rows gives no height at all, and a grid held to the viewport over a fixed `MinHeight`, which knows nothing of how tall the parts are and, in the narrowest window, cut off the IP ranges page's add form, the domains page's alias form and, in German, the rules page's last action button, and left the domains page's alias list and the rules page's actions no height. The domains page fills two columns, the domains card one and the accounts card and the row of alias and list cards the other, three to two as its grid's rows were; the rules page fills with its list of rules and the row of criteria and actions, one to 1.3. `Services/PageFill.cs` is the arithmetic, held by `PageFillTests`.

Parts side by side keep a minimum width, and go one above the other where they would not (`Services/SideBySide.cs`, held by `SideBySideTests`): the domains page's columns, which need 280 and 400 pixels and share the width two to three, so that below a page of 700 pixels the domains card goes above the accounts card in the one `FillPanel`; its alias and list cards, 280 each; and the rules page's criteria and actions, 340 each, the IF caption and the 230-pixel match list. Side by side in the narrowest window (760 by 520) the domains page's columns were 268 and 232 pixels wide, *Create account* ran off its card and the alias and list cards were 104 and 116, and the rules page's match list ran off its card there and in the account dialog's Rules tab. A row of a list built from a `ListView` is the list's width, never wider (`ScrollViewer.HorizontalScrollBarVisibility="Disabled"`), its name in `GridTrimmedText`, so that a long name ends in an ellipsis, whole in its tool tip, and the row's buttons stay on it: scrolled sideways, one long domain name put every domain's *Properties* and *Delete* off the list's right edge. Where a form's buttons do not fit beside its box, they go on a line of their own under it, as the domains page's *Generate* and *Create account* do below a password box of 120 pixels.

Grids are styled implicitly in `App.xaml`: the `DataGrid` chains to WPF-UI's own and has no grid lines, column headers only, rows at least `AppNavRowHeight` tall and a full-row selection; the row, cell and header styles are the ones the grids already had (a hairline under each row, the hover grey, semibold low-key headers), the row's selection being the one described under *Selection* above. A grid built in code for a dialog's results - the Active Directory picker's - is a read-only `DataGrid` on the same styles, its columns sharing the width in stars through `GridStyles.Trimmed`, never a `ListView` with a `GridView`, which takes WPF's stock look and not the theme's. A number column sets `ElementStyle="{StaticResource GridNumberText}"` and `HeaderStyle="{StaticResource GridNumberHeader}"`, which right-align its cells and its header. A grid's two scroll bars must not depend on each other: where its rows fit only without a horizontal bar and layout rounding makes its columns a fraction of a pixel wider than the grid, as it can at 125 and 175 %, the bars take turns for ever, the page's layout never settles and the Control Panel takes no more input, as the rules page's lists did. A grid of one column whose text ends in an ellipsis does not scroll sideways (`ScrollViewer.HorizontalScrollBarVisibility="Disabled"`), and one whose columns keep their widths and scroll sideways keeps its vertical bar (`ScrollViewer.VerticalScrollBarVisibility="Visible"`), as the rules page's do.

## Rules

- **No new text without its seventeen translations.** Every caption is looked up by its English text (`Services/Loc.cs`), and `build/check-localisation.py`, `build/check-mnemonics.py` and `build/check-catalogues.py` fail the build on a caption missing from any catalogue, on a caption without an Alt key or with a colliding one, and on a translation that drops a token the reader types back. The cheapest redesign changes no text; a component's defaults are the only texts the scaffold adds.
- **No implicit TextBlock style**, for the reason under *Type*.
- **Brushes by DynamicResource or by key**, never by a held instance, for the reason under *Colour*.
- **Colour, shape and word** for every status, and the word first in the accessible name.
- **Every element keeps its automation name.** A `PageHeader` names the page, a `Card` its group, a `FieldRow` its editor; the ids the shell and the pages carry (`nav-<key>`, `breadcrumb-*`, `dashboard-*`) are what `build/capture-cp.ps1` and any automation hold on to and do not change.
- **One page, one `ScrollViewer`, padded with `AppPagePadding`.**
- **A tour is never the only way to do anything, and never blocks the product while it runs.** See *Tours* below.

## Tours

A tour teaches a page while somebody uses it. One mechanism, the same shape in all three surfaces - this
Control Panel, the browser Control Deck (`hmailserver/installation/WebAdmin/index.html`) and the webmail
(`Server/Common/Util/Portal.html` and `Portal.js`) - so that the three can be reasoned about, and read in
the store, with one explanation.

**A step** is an element, one sentence, an optional condition that ends the step when the thing has
happened, and what to do when the element is not there. The element is named by its automation id in the
Control Panel and by its element id (or, in the Deck, a stable selector) in the two pages - never by a
position, because a step pinned to "the third button" survives nothing. A step that cannot find its element
is **skipped**, unless it is marked as one the rest of the tour is meaningless without, in which case the
tour ends *saying so*. A step never acts: it does not press the button it points at and it does not fill
the field. The only thing it does besides pointing is open the page or view its element lives on, because
an element on a page nobody has opened is in no visual tree to be found in.

**A tour** is an ordered list of steps with an id, a name and a version. The id is what the finished record
is kept under, so renaming one abandons every reader's record of having done it; the version is raised when
the steps change enough that somebody who finished the old tour has not seen the new one.

**Where it is kept.** Which tours this person has finished, and where they stopped in one they left, as one
line: `firstrun=1|firstrun@3v1` - the finished tours with the version each was finished at, then the resume
points as *step* `v` *version*. The Control Panel keeps it in `HKCU\Software\hMailServer\ControlPanel`
under `TourProgress`, beside the window bounds, the palette's history and the sidebar's collapsed state; the
Deck in `localStorage` under `hmsTours`; the webmail in the account's preferences under `tours`
(`/api/v1/me/preferences`), so that a reader who starts the walk on a laptop finishes it on a phone. Every
kind of damage to the stored line is survivable and silent in all three: the worst it can cost is being
offered a tour twice.

**The rules**, which are the feature rather than decoration:

- **Never modal, and never in the way.** The ring takes no pointer events and the card is the only thing on
  its layer that does, so every control underneath - the very one being pointed at - stays usable while a
  tour runs. A tour is never the only way to do anything.
- **Never takes the keyboard.** Nothing is focused when a step opens, and the focus the tour found is put
  back when it ends. The card's buttons are ordinary tab stops; `F6` reaches them from anywhere, and
  `Escape` ends the tour **only** while the keyboard is already inside the card - everywhere else Escape
  still belongs to the page.
- **No Alt keys on the card.** The one place in this application where a caption carries no access key. The
  card floats over whichever page the step is on, so any key it claimed would sooner or later be a key that
  page already owns, and an access key that does the wrong thing on one page in forty-seven is worse than
  none.
- **The highlight is not colour alone.** A solid ring in the accent and a dashed one inside it in the text
  colour, so the highlight survives greyscale, colour blindness and High Contrast - where the accent becomes
  the system highlight and the dashes are what still separate the ring from an ordinary focus rectangle.
- **The sentence is announced.** The card is a polite live region and the Control Panel raises a UI
  Automation notification with the sentence as each step opens, so the tour is usable without sight and
  without the focus being taken to make that happen.
- **It is the change that ends a step, not the state.** A step whose condition was already true when the
  reader arrived stays until they press Next; somebody who already has a domain should read the sentence
  about domains rather than watch the step vanish before they have read it.
- **Every ending says something.** Finished, left, or stopped because a step it needed was not on the
  screen - each is a sentence. Silence after a tour that stopped on its own is how somebody decides the
  feature is broken.
- **Every sentence goes through the catalogues**, like every other text: seventeen languages here, twenty in
  the Deck, twenty-three in the webmail.

**Where the logic lives.** In the Control Panel, everything that can be decided without a window is in
`Services/Tour.cs` (the steps and the catalogue), `Services/TourSession.cs` (the stepper - which step is
showing, what Next and Back do, when and why the tour ended) and `Services/TourProgress.cs` (the record),
all three compiled into `ControlPanel.Core` and tested without WPF, exactly as `ConnectionStatus` and
`SettingsPageStatus` are. `MainWindow.Tour.cs` is the half that genuinely needs a window: opening the page a
step is on, finding the control by its automation id, keeping the ring on it while the page moves
underneath, and answering the conditions from the server. The two browser pages keep the same division by
convention rather than by compilation, and `build/deck-script-test.js` and `build/portal-script-test.js`
execute theirs.

**The ways in.** A tour is offered on the Welcome page (a button in the header and, until the walk has been
finished, a notice above the intents), in the Ctrl+K palette as a row of its own above the page list, and by
a help button in the header of any page a tour visits - which starts that tour at its first stop *on that
page*, so the button teaches the page it is on. The Deck's is the `?` button in its header and the
webmail's is *Show me around* in the account menu; both behave the same way.

## Migrating a page

The dashboard (`Views/DashboardView.xaml`) is the pattern for a XAML page and the sign-in page (`Views/ConnectView.xaml`) for a form. A page changes in this order: the `ScrollViewer` takes `AppPagePadding`; the title and subtitle become a `PageHeader` (its `Subtitle` is set from code where the page updates it - the dashboard writes the time of the last refresh there); the page's buttons move into the header's `Actions`; every `Border` with the `Card` style becomes a `Card`, with its heading as the `Title`; a state shown as a mark and a word becomes a `StatusPill`; an error line becomes an `InlineNotice`; a label above an editor becomes a `FieldRow`, and the editor loses its placeholder where the caption says the same. A code-built page does the same with `new Card { Title = L("..."), Content = ... }`. Nothing about what the page reads or writes changes, and the tests that read the page's source for its settings (`SettingClaimsTests`, `SettingsSearchIndexTests`) keep passing because the setting keys and labels are not what moves.

## Where the migration stands

Every page is on the frame since 15 September 2026, in three waves after the first: the twelve remaining XAML pages and the code-built pages (wave 2), `ServerSettingsView` with its 31 tabs and 53 cards (wave 3), and `FeatureSettingsView` with all eighteen dialogs, the message boxes and the palette (wave 4). No page title, no tab header, no setting caption and no automation id moved in any of them, which is what kept the search index, the Ctrl+K palette and `SettingClaimsTests`/`SettingsSearchIndexTests` passing throughout.

What the waves added beside the components, because a page needed it and a component did not offer it:

* `Services/StatusText.cs` and `Services/GridStyles.cs` - the second because the keyed grid styles are XAML resources and a third of the pages build their columns in C#; `GridStyles.Number(column)` is the C# way to say `GridNumberText`. Both are candidates for the scaffold.
* `Services/SettingsPageStatus.cs` and `Services/ServerSettingsPageStatus.cs` - the word and the severity of a settings page's pill, per state. Two types because there are two lifecycles: an hMailServer.INI page is read and written over COM and applies on save (*Applied*), except the keys the server marks reinitialize - which the window's **Apply now** takes (*Saved*) - or restart (*Saved, Restarting, RestartFailed*) ([SettingsApply.md](SettingsApply.md)); against a server from before a save could publish it says a restart applies them, as it always did (*Editing, Unavailable, NotConnected* around those), and the server settings page is read and written over COM - and over the REST API for the settings no COM property carries - and takes effect as it saves (*Clean, Unsaved, Saving, Saved, PartlySaved*). Both are WPF-free and both are held to a distinct word per state by a test.
* `Views/DialogFields.cs` - `Field`, the editors, and the two tab-sizing helpers described above.
* `Services/DialogLayout.cs` - the arithmetic that sizes a tabbed dialog within the work area.

What the waves asked for and did not get, in the order they are worth doing:

1. **`FitTabs`, `FillTabs` and the framed-prompt helper belong in `FluentDialogWindow`.** They are in `DialogFields` only because `UseFrame` is protected and the static prompts build a bare window.
2. **`Card` has no status slot and no heading level.** `PageHeader` has a status slot; a card that needs one puts the pill in the first row of its content. A settings card *is* a section, so it is a title-less `Card` wrapping a `SettingsSection` to get the level-2 heading - one unnamed group around one named one.
3. **`SettingsSection` and `FieldRow` carry a default margin** (`AppSectionGap`, `AppFieldGap`) that a caller inside a card has to zero. A default most callers clear is in the wrong place.
4. **`FieldRow` cannot say "the check box is its own caption".** Passing `Label = null` leaves the row's automation group unnamed.
5. **`Toolbar` never puts its actions under the search box, and a few controls keep a width of their own.** The search box keeps 220 pixels and the actions have what is left, so in a page much narrower than the narrowest window's 512 pixels - the help pane open beside it leaves about 250 - a combo box or a button wider than that runs off the toolbar's right edge, as the rules page's scope list does; at that width the rules page's 230-pixel match list is wider than its card, and the IP ranges page's permission boxes are cut at their right edges. Every list and every form can still be scrolled to.

Two things are deliberately not migrated and are not defects: `AccessibleChartCard`, `AccountTwoFactorPanel` and `AppPasswordsPanel` still use the old `Card` Border style, which is why `App.xaml` keeps it - a title-less `Card` draws the same surface, so they are identical on screen; and the `LogsView`'s severity palette (`Log*Brush`) is the log viewer's own and is not a design token.
