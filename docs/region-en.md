<sub>[Español](region-es.md)</sub>

# How to play if your country doesn't have Xbox Cloud Gaming

## In-app option: Access country

1. On the **Possible regional restriction** card, use **LEFT/RIGHT** to choose **Automatic**, **United States**, **Brazil**, **Japan**, **Korea** or **Poland**. You can also open **OPTIONS > Access** using **L1/R1**.
2. Press **X Try access** on the card or **X Save** in Settings. The first time you confirm a non-Automatic country, read the warning and press **X I understand, continue** or **CIRCLE Cancel**.
3. The card queries the catalog or Fortnite again using your already-authorized Microsoft sign-in; it does not ask for the code again. **CIRCLE** cancels the query. Settings applies your choice to the next query or game.
4. The app remembers the country and acknowledgement in `/data/xcloud4/access-v1.bin`, without account data. Select **Automatic** in **OPTIONS > Access** to revert. The initial **Choose your server** panel remains server-only.
5. **TRIANGLE More help** on the card opens this guide's QR.

This option declares another country only in the cloud-service login request. It is not a VPN, does not change your account's country or the game server, and does not provide a subscription. Access is not guaranteed. It does not resolve earlier Xbox-profile, account-country, age or network errors; those retain their specific messages.

**Using a different access country may go against Xbox's terms, and Microsoft could restrict your account. Use it at your own risk.**

## Alternative: account and server

Some users report that this method works. It is not guaranteed and may stop working at any time:

1. While connected to a VPN in a country where Xbox Cloud Gaming is available, create a new Microsoft account with that country as its region.
2. Sign in once at xbox.com with that account to create the Xbox profile (gamertag). To play the catalog you need Game Pass Ultimate on that account; for Fortnite the free account is enough.
3. On the PS4 (without VPN), sign in to XCloud4 with that account.
4. In OPTIONS > Server, choose the server closest to you. After one game on each you'll see its real ms; pick the lowest.

Risks: creating an account with a country that isn't yours may go against Microsoft and Xbox terms. Your account could be suspended, and there may be problems with payments and subscriptions from another region. You do this at your own risk; XCloud4 is not responsible.

Official list of countries with Xbox Cloud Gaming: https://www.xbox.com/regions

With **Access country: Automatic**, XCloud4 adds no location declaration to the login. With another country selected, it declares that country's reference IP only when requesting cloud access; the connection retains its real origin.

**Disclaimer:** XCloud4 is an independent project, not affiliated with Microsoft, Xbox or Sony. It is provided as is, without warranty. You use it at your own risk: the project is not responsible for problems with your console, account or subscriptions. Use XCloud4 in accordance with Microsoft and Xbox terms of service.
