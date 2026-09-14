import 'package:flutter_test/flutter_test.dart';
import 'package:gui/main.dart';

void main() {
  testWidgets('Miner dashboard smoke test', (WidgetTester tester) async {
    await tester.pumpWidget(const BtcMinerApp());
    expect(find.text('BITCOIN STRATUM V1 MINER'), findsOneWidget);
  });
}
