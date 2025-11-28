# 🧪 Local Cloud Function Testing

This guide explains how to run and test the `everframe-function.py` on your local machine before deploying it to Google Cloud. This allows for faster debugging and iteration.

We will use the Functions Framework for Python, which emulates the GCP Cloud Functions environment locally.

---

## 📋 Prerequisites

1.  **Python 3.9+**: Ensure you have a compatible Python version installed.
2.  **Google Cloud SDK**: Make sure you have `gcloud` installed and configured.
3.  **Project Dependencies**: You'll need to install the Python libraries your function depends on.

---

## ⚙️ Setup & Execution

### Step 1: Install Dependencies

It's recommended to use a virtual environment. Install the necessary libraries from your terminal:

```bash
pip install functions-framework google-cloud-storage google-cloud-pubsub google-cloud-secretmanager google-cloud-aiplatform
```

### Step 2: Authenticate with GCP

Your function needs to communicate with GCP services (Storage, Vertex AI, etc.). To grant it permission on your local machine, run the Application Default Credentials login command:

```bash
gcloud auth application-default login
```

This will open a browser window for you to log in with your Google account.

### Step 3: Run the Local Server

Navigate to the `cloud` directory and start the functions framework server. This command tells the framework to load your `everframe-function.py` file and target the `hello_pubsub` function, treating it as a CloudEvent function (like one triggered by Pub/Sub).

```bash
# In your terminal, from the 'cloud' directory:
functions-framework --target hello_pubsub --signature-type cloudevent
```

You should see output indicating the server is running, typically on `http://localhost:8080`.

### Step 4: Trigger the Function

The function expects a Pub/Sub message, which is sent as an HTTP POST request with a specific JSON payload.

1.  **Create a test payload file.** The function is only triggered by the message and doesn't use its content, so a minimal payload is sufficient. The `data` field must be a base64-encoded string. `e30=` is the base64 encoding of an empty JSON object `{}`.

    Create a file named `payload.json`:
    ```json
    {
      "message": {
        "data": "e30=",
        "messageId": "local-test-123"
      }
    }
    ```

2.  **Send the request.** Open a **new terminal window** and use `curl` to send the payload to your running function:

    ```bash
    curl -X POST http://localhost:8080 \
      -H "Content-Type: application/json" \
      -d @payload.json
    ```

### Step 5: Observe the Output

Switch back to the terminal where the `functions-framework` is running. You will see the output from your function as it executes, including:

- The initial "Received Pub/Sub message..." print statement.
- A list of blobs found in the bucket.
- "Analyzing..." and "Generating..." messages from the AI steps.
- The final success message with the Pub/Sub ID.

If you encounter any errors, they will be printed here, allowing you to debug your code directly in your local environment.

---

By following these steps, you can fully test the function's interaction with live GCP services without needing to deploy it repeatedly.
